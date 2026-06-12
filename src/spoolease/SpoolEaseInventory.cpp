#include "SpoolEaseInventory.hpp"

#include "SpoolEaseConfig.hpp"
#include "SpoolEaseLog.hpp"
#include "SpoolEaseStatus.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/x509.h>

#include <wx/app.h>
#include <wx/window.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace Slic3r { namespace SpoolEase {

namespace {

constexpr auto poll_interval = std::chrono::seconds(5);

std::once_flag s_curl_init_once;

struct ParseSlotsResult
{
    std::optional<std::unordered_map<std::string, SlotInventory>> cache;
    std::string                                                   error;
};

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

const char* curl_code_name(CURLcode code)
{
    switch (code) {
    case CURLE_OK: return "CURLE_OK";
    case CURLE_UNSUPPORTED_PROTOCOL: return "CURLE_UNSUPPORTED_PROTOCOL";
    case CURLE_URL_MALFORMAT: return "CURLE_URL_MALFORMAT";
    case CURLE_COULDNT_RESOLVE_PROXY: return "CURLE_COULDNT_RESOLVE_PROXY";
    case CURLE_COULDNT_RESOLVE_HOST: return "CURLE_COULDNT_RESOLVE_HOST";
    case CURLE_COULDNT_CONNECT: return "CURLE_COULDNT_CONNECT";
    case CURLE_READ_ERROR: return "CURLE_READ_ERROR";
    case CURLE_OUT_OF_MEMORY: return "CURLE_OUT_OF_MEMORY";
    case CURLE_OPERATION_TIMEDOUT: return "CURLE_OPERATION_TIMEDOUT";
    case CURLE_SSL_CONNECT_ERROR: return "CURLE_SSL_CONNECT_ERROR";
    case CURLE_ABORTED_BY_CALLBACK: return "CURLE_ABORTED_BY_CALLBACK";
    case CURLE_BAD_FUNCTION_ARGUMENT: return "CURLE_BAD_FUNCTION_ARGUMENT";
    case CURLE_GOT_NOTHING: return "CURLE_GOT_NOTHING";
    case CURLE_SEND_ERROR: return "CURLE_SEND_ERROR";
    case CURLE_RECV_ERROR: return "CURLE_RECV_ERROR";
    case CURLE_SSL_CERTPROBLEM: return "CURLE_SSL_CERTPROBLEM";
    case CURLE_SSL_CIPHER: return "CURLE_SSL_CIPHER";
    case CURLE_PEER_FAILED_VERIFICATION: return "CURLE_PEER_FAILED_VERIFICATION";
    case CURLE_SSL_ENGINE_INITFAILED: return "CURLE_SSL_ENGINE_INITFAILED";
    case CURLE_SSL_CACERT_BADFILE: return "CURLE_SSL_CACERT_BADFILE";
    default: return "CURLE_UNKNOWN";
    }
}

bool is_tls_error(CURLcode code)
{
    switch (code) {
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_INITFAILED:
        return true;
    default:
        return false;
    }
}

std::string clean_curl_error(const std::array<char, CURL_ERROR_SIZE>& error, CURLcode code)
{
    const char* begin = error.data();
    const char* end = std::find(begin, begin + error.size(), '\0');
    std::string message(begin, end);
    if (message.empty())
        message = curl_easy_strerror(code);
    return message;
}

long request_size_bytes(CURL* curl)
{
    long request_bytes = 0;
#ifdef CURLINFO_REQUEST_SIZE
    curl_easy_getinfo(curl, CURLINFO_REQUEST_SIZE, &request_bytes);
#endif
    return request_bytes;
}

long long curl_time_ms(CURL* curl, CURLINFO info)
{
    double total_seconds = 0.0;
    if (curl_easy_getinfo(curl, info, &total_seconds) == CURLE_OK && total_seconds >= 0.0)
        return static_cast<long long>(total_seconds * 1000.0 + 0.5);
    return 0;
}

std::string curl_string_info(CURL* curl, CURLINFO info)
{
    char* value = nullptr;
    if (curl_easy_getinfo(curl, info, &value) == CURLE_OK && value)
        return value;
    return {};
}

long curl_long_info(CURL* curl, CURLINFO info)
{
    long value = 0;
    curl_easy_getinfo(curl, info, &value);
    return value;
}

struct CurlDiagnostics
{
    std::string primary_ip;
    long        primary_port{0};
    std::string local_ip;
    long        local_port{0};
    long long   namelookup_ms{0};
    long long   connect_ms{0};
    long long   appconnect_ms{0};
    long long   total_ms{0};
};

CurlDiagnostics curl_diagnostics(CURL* curl)
{
    CurlDiagnostics diagnostics;
#if LIBCURL_VERSION_NUM >= 0x071300
    diagnostics.primary_ip = curl_string_info(curl, CURLINFO_PRIMARY_IP);
#endif
#if LIBCURL_VERSION_NUM >= 0x071500
    diagnostics.primary_port = curl_long_info(curl, CURLINFO_PRIMARY_PORT);
    diagnostics.local_ip = curl_string_info(curl, CURLINFO_LOCAL_IP);
    diagnostics.local_port = curl_long_info(curl, CURLINFO_LOCAL_PORT);
#endif
    diagnostics.namelookup_ms = curl_time_ms(curl, CURLINFO_NAMELOOKUP_TIME);
    diagnostics.connect_ms = curl_time_ms(curl, CURLINFO_CONNECT_TIME);
    diagnostics.appconnect_ms = curl_time_ms(curl, CURLINFO_APPCONNECT_TIME);
    diagnostics.total_ms = curl_time_ms(curl, CURLINFO_TOTAL_TIME);
    return diagnostics;
}

std::string curl_diagnostics_log(const CurlDiagnostics& diagnostics)
{
    std::ostringstream out;
    out << " primary_ip=\"" << (diagnostics.primary_ip.empty() ? "-" : diagnostics.primary_ip) << "\""
        << " primary_port=" << diagnostics.primary_port
        << " local_ip=\"" << (diagnostics.local_ip.empty() ? "-" : diagnostics.local_ip) << "\""
        << " local_port=" << diagnostics.local_port
        << " namelookup_ms=" << diagnostics.namelookup_ms
        << " connect_ms=" << diagnostics.connect_ms
        << " appconnect_ms=" << diagnostics.appconnect_ms
        << " total_ms=" << diagnostics.total_ms;
    return out.str();
}

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string tls_verify_reason(long verify_result)
{
    const char* reason = X509_verify_cert_error_string(verify_result);
    return reason ? std::string(reason) : std::string();
}

std::optional<std::string> fetch_slots_json(const ConsoleConfig& config)
{
    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        SPOOLEASE_LOG(error) << "SpoolEase: API curl init failed";
        set_live_status_error("inventory_api", "API error: failed to initialize HTTP client.");
        return std::nullopt;
    }

    std::string body;
    std::array<char, CURL_ERROR_SIZE> error{};
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
    const long request_bytes = request_size_bytes(curl);
    const CurlDiagnostics diagnostics = curl_diagnostics(curl);

    char* content_type_raw = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_raw);
    const std::string content_type = content_type_raw ? std::string(content_type_raw) : std::string();

    long verify_result = 0;
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        const std::string error_message = clean_curl_error(error, result);
        SPOOLEASE_LOG(warning) << "SpoolEase: API request failed: method=GET url=" << url
                               << " status=" << status
                               << " curl_code=" << static_cast<int>(result)
                               << " curl_name=" << curl_code_name(result)
                               << " error=\"" << error_message << "\""
                               << " request_bytes=" << request_bytes
                               << " request_body_bytes=0"
                               << " response_body_bytes=" << body.size()
                               << curl_diagnostics_log(diagnostics);

        if (is_tls_error(result)) {
            SPOOLEASE_LOG(warning) << "SpoolEase: API TLS verification failed: url=" << url
                                   << " curl_code=" << static_cast<int>(result)
                                   << " curl_name=" << curl_code_name(result)
                                   << " verify_result=" << verify_result
                                   << " verify_reason=\"" << tls_verify_reason(verify_result) << "\""
                                   << " error=\"" << error_message << "\""
                                   << curl_diagnostics_log(diagnostics);
        }
        set_live_status_error("inventory_api", "API error: " + error_message);
        return std::nullopt;
    }

    if (status != 200) {
        SPOOLEASE_LOG(warning) << "SpoolEase: API returned non-200: method=GET url=" << url
                               << " status=" << status
                               << " request_bytes=" << request_bytes
                               << " request_body_bytes=0"
                               << " response_body_bytes=" << body.size()
                               << curl_diagnostics_log(diagnostics);
        set_live_status_error("inventory_api", "API error: HTTP " + std::to_string(status) + ".");
        return std::nullopt;
    }

    if (!content_type.empty() && lower_copy(content_type).find("json") == std::string::npos) {
        SPOOLEASE_LOG(warning) << "SpoolEase: API response content type unexpected: url=" << url
                               << " status=" << status
                               << " content_type=\"" << content_type << "\""
                               << " response_body_bytes=" << body.size();
    }

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

ParseSlotsResult parse_slots_json(const std::string& body)
{
    ParseSlotsResult result;
    std::unordered_map<std::string, SlotInventory> cache;
    const nlohmann::json root = nlohmann::json::parse(body);
    const auto printers_it = root.find("printers");
    if (printers_it == root.end() || !printers_it->is_array()) {
        result.error = "missing or invalid root.printers array";
        return result;
    }

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

    result.cache = std::move(cache);
    return result;
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

    void refresh_now()
    {
        start();
        m_wake = true;
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
        SPOOLEASE_LOG(info) << "SpoolEase: API loop started";
        while (!m_stop) {
            poll_once();
            const auto until = std::chrono::steady_clock::now() + poll_interval;
            while (!m_stop && !m_wake.exchange(false) && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        SPOOLEASE_LOG(info) << "SpoolEase: API loop stopped";
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
        const std::optional<ConsoleConfig> config = console_config(false, "api_loop");
        if (!config.has_value()) {
            note_config_key({});
            clear_live_status("inventory_api");
            return;
        }

        note_config_key(config_key(*config));

        const std::optional<std::string> body = fetch_slots_json(*config);
        if (!body.has_value())
            return;

        const std::string url = api_url(*config);
        try {
            ParseSlotsResult parsed = parse_slots_json(*body);
            if (parsed.cache.has_value()) {
                replace_cache(std::move(*parsed.cache));
                clear_live_status("inventory_api");
            } else {
                SPOOLEASE_LOG(warning) << "SpoolEase: API JSON schema invalid: url=" << url
                                       << " response_body_bytes=" << body->size()
                                       << " reason=\"" << parsed.error << "\"";
                set_live_status_error("inventory_api", "API response schema is invalid: " + parsed.error);
            }
        } catch (const std::exception& e) {
            SPOOLEASE_LOG(warning) << "SpoolEase: API JSON parse failed: url=" << url
                                   << " response_body_bytes=" << body->size()
                                   << " error=\"" << e.what() << "\"";
            set_live_status_error("inventory_api", std::string("API response is not valid JSON: ") + e.what());
        } catch (...) {
            SPOOLEASE_LOG(warning) << "SpoolEase: API JSON parse failed: url=" << url
                                   << " response_body_bytes=" << body->size()
                                   << " error=\"unknown error\"";
            set_live_status_error("inventory_api", "API response is not valid JSON.");
        }
    }

private:
    std::mutex m_thread_mutex;
    std::mutex m_cache_mutex;
    std::thread m_thread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_wake{false};
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

void refresh_inventory_now()
{
    poller().refresh_now();
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
