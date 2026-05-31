#include "SpoolEaseInventory.hpp"

#include "SpoolEaseConfig.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/window.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace Slic3r { namespace SpoolEase {

namespace {

constexpr auto poll_interval = std::chrono::seconds(5);

std::once_flag s_curl_init_once;

std::string cache_key(const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
{
    return printer_serial + '\x1f' + ams_id + '\x1f' + slot_id;
}

std::string config_key(const ConsoleConfig& config)
{
    return config.address + '\x1f' + config.api_token + '\x1f' + config.ca_cert_pem;
}

size_t write_callback(void* data, size_t size, size_t count, void* user)
{
    auto* body = static_cast<std::string*>(user);
    body->append(static_cast<const char*>(data), size * count);
    return size * count;
}

std::string api_url(const ConsoleConfig& config)
{
    std::string address = config.address;
    while (!address.empty() && address.back() == '/')
        address.pop_back();
    return "https://" + address + "/api/internal/printers/slots";
}

std::optional<std::string> fetch_slots_json(const ConsoleConfig& config)
{
    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl)
        return std::nullopt;

    std::string body;
    std::string error(CURL_ERROR_SIZE, '\0');
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + config.api_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    const std::string url = api_url(config);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error.data());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 4L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

    if (!config.ca_cert_pem.empty()) {
#if LIBCURL_VERSION_NUM >= 0x074D00
        struct curl_blob ca_blob;
        ca_blob.data = const_cast<char*>(config.ca_cert_pem.data());
        ca_blob.len = config.ca_cert_pem.size();
        ca_blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
#else
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
#endif
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK || status != 200)
        return std::nullopt;

    return body;
}

std::string json_string(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_string())
        return {};
    return it->get<std::string>();
}

std::optional<float> json_float(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_number())
        return std::nullopt;
    return it->get<float>();
}

bool cache_equals(const std::unordered_map<std::string, SlotInventory>& lhs, const std::unordered_map<std::string, SlotInventory>& rhs)
{
    if (lhs.size() != rhs.size())
        return false;

    for (const auto& item : lhs) {
        const auto it = rhs.find(item.first);
        if (it == rhs.end())
            return false;
        if (item.second.spool_id != it->second.spool_id || item.second.weight_net != it->second.weight_net)
            return false;
    }

    return true;
}

std::optional<std::unordered_map<std::string, SlotInventory>> parse_slots_json(const std::string& body)
{
    std::unordered_map<std::string, SlotInventory> cache;
    const nlohmann::json root = nlohmann::json::parse(body);
    const auto printers_it = root.find("printers");
    if (printers_it == root.end() || !printers_it->is_array())
        return std::nullopt;

    for (const auto& printer : *printers_it) {
        if (!printer.is_object() || json_string(printer, "kind") != "Bambu")
            continue;

        const std::string printer_serial = json_string(printer, "native_id");
        if (printer_serial.empty())
            continue;

        const auto groups_it = printer.find("slot_groups");
        if (groups_it == printer.end() || !groups_it->is_array())
            continue;

        for (const auto& group : *groups_it) {
            if (!group.is_object())
                continue;

            const std::string ams_id = json_string(group, "native_id");
            if (ams_id.empty())
                continue;

            const auto slots_it = group.find("slots");
            if (slots_it == group.end() || !slots_it->is_array())
                continue;

            for (const auto& slot : *slots_it) {
                if (!slot.is_object())
                    continue;

                const std::string slot_id = json_string(slot, "native_id");
                const std::string spool_id = json_string(slot, "spool_id");
                if (slot_id.empty() || spool_id.empty())
                    continue;

                cache[cache_key(printer_serial, ams_id, slot_id)] = SlotInventory{spool_id, json_float(slot, "weight_net")};
            }
        }
    }

    return cache;
}

void request_ui_refresh()
{
    if (!wxTheApp)
        return;

    wxTheApp->CallAfter([]() {
        if (!wxTheApp)
            return;
        if (wxWindow* top = wxTheApp->GetTopWindow())
            top->Refresh(true);
    });
}

class InventoryPoller
{
public:
    ~InventoryPoller()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
    }

    void start()
    {
        std::lock_guard<std::mutex> lock(m_thread_mutex);
        if (m_started)
            return;

        m_started = true;
        m_thread = std::thread([this]() { run(); });
    }

    std::optional<SlotInventory> lookup(const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
    {
        if (printer_serial.empty() || ams_id.empty() || slot_id.empty())
            return std::nullopt;

        std::lock_guard<std::mutex> lock(m_cache_mutex);
        const auto it = m_cache.find(cache_key(printer_serial, ams_id, slot_id));
        return it == m_cache.end() ? std::nullopt : std::optional<SlotInventory>(it->second);
    }

private:
    void run()
    {
        while (!m_stop) {
            poll_once();
            const auto until = std::chrono::steady_clock::now() + poll_interval;
            while (!m_stop && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void note_config_key(const std::string& key)
    {
        bool cleared = false;
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            if (m_config_key == key)
                return;

            m_config_key = key;
            if (!m_cache.empty()) {
                m_cache.clear();
                cleared = true;
            }
        }

        if (cleared)
            request_ui_refresh();
    }

    void replace_cache(std::unordered_map<std::string, SlotInventory>&& cache)
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            changed = !cache_equals(m_cache, cache);
            if (!changed)
                return;
            m_cache = std::move(cache);
        }

        if (changed)
            request_ui_refresh();
    }

    void poll_once()
    {
        const std::optional<ConsoleConfig> config = console_config(false);
        if (!config.has_value() || config->address.empty() || config->api_token.empty()) {
            note_config_key({});
            return;
        }

        note_config_key(config_key(*config));

        const std::optional<std::string> body = fetch_slots_json(*config);
        if (!body.has_value())
            return;

        try {
            std::optional<std::unordered_map<std::string, SlotInventory>> parsed = parse_slots_json(*body);
            if (parsed.has_value())
                replace_cache(std::move(*parsed));
        } catch (...) {
        }
    }

private:
    std::mutex m_thread_mutex;
    std::mutex m_cache_mutex;
    std::thread m_thread;
    std::atomic_bool m_stop{false};
    bool m_started{false};
    std::string m_config_key;
    std::unordered_map<std::string, SlotInventory> m_cache;
};

InventoryPoller& poller()
{
    static InventoryPoller instance;
    return instance;
}

} // namespace

void start_inventory_polling()
{
    poller().start();
}

std::optional<SlotInventory> slot_inventory(const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
{
    return poller().lookup(printer_serial, ams_id, slot_id);
}

std::string display_spool_id(const std::string& spool_id)
{
    if (spool_id.size() <= 4)
        return spool_id;
    return spool_id.substr(spool_id.size() - 4);
}

std::string display_weight(std::optional<float> weight)
{
    if (!weight.has_value())
        return "-";

    std::ostringstream out;
    out << std::fixed << std::setprecision(weight.value() < 10.f ? 1 : 0) << weight.value() << "g";
    return out.str();
}

}} // namespace Slic3r::SpoolEase
