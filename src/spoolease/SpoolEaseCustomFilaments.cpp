#include "SpoolEaseCustomFilaments.hpp"

#include "SpoolEaseConfig.hpp"
#include "SpoolEaseLog.hpp"
#include "SpoolEaseWebPage.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/font.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace Slic3r { namespace SpoolEase {

namespace {

constexpr size_t custom_filaments_max_chars = 4000;

std::once_flag s_curl_init_once;

struct PostResult
{
    bool        ok{false};
    long        status{0};
    std::string error;
    std::string response;
};

struct FetchResult
{
    bool        ok{false};
    long        status{0};
    std::string error;
    std::string response;
    std::string custom_filaments;
};

struct CustomFilamentRow
{
    std::string filament_id;
    std::string preset_name;
    std::string display_name;
    int         nozzle_temp_low{0};
    int         nozzle_temp_high{0};
    std::string filament_type;
};

std::string to_utf8(const wxString& text)
{
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer.data() ? std::string(buffer.data()) : std::string();
}

std::string trim_copy(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
    return value;
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string normalize_custom_filaments_text(std::string text)
{
    replace_all(text, "\r\n", "\n");
    replace_all(text, "\r", "\n");
    return trim_copy(text);
}

std::string csv_escape(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos)
        return value;

    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (char ch : value) {
        if (ch == '"')
            out.push_back('"');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

std::string name_before_at(std::string name)
{
    const size_t at = name.find('@');
    if (at != std::string::npos)
        name = name.substr(0, at);
    return trim_copy(name);
}

std::optional<int> first_int(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionInts>(key);
    if (!option || option->values.empty())
        return std::nullopt;
    return option->values.front();
}

std::optional<std::string> first_string(const DynamicPrintConfig& config, const char* key)
{
    const auto* option = config.option<ConfigOptionStrings>(key);
    if (!option || option->values.empty())
        return std::nullopt;
    return option->values.front();
}

std::string build_custom_filaments_csv()
{
    PresetBundle* preset_bundle = Slic3r::GUI::wxGetApp().preset_bundle;
    if (!preset_bundle)
        return {};

    std::map<std::string, CustomFilamentRow> rows;

    for (const Preset& preset : preset_bundle->filaments.get_presets()) {
        if (!preset.is_user() || preset.is_project_embedded || preset.filament_id.empty() || !preset.base_id.empty())
            continue;

        const std::optional<int> nozzle_temp_low = first_int(preset.config, "nozzle_temperature_range_low");
        const std::optional<int> nozzle_temp_high = first_int(preset.config, "nozzle_temperature_range_high");
        const std::optional<std::string> filament_type = first_string(preset.config, "filament_type");
        if (!nozzle_temp_low.has_value() || !nozzle_temp_high.has_value() || !filament_type.has_value()) {
            SPOOLEASE_LOG(warning) << "SpoolEase: custom filament skipped due to missing required fields: filament_id=" << preset.filament_id;
            continue;
        }

        auto existing = rows.find(preset.filament_id);
        if (existing != rows.end() && existing->second.preset_name <= preset.name)
            continue;

        rows[preset.filament_id] = CustomFilamentRow{
            preset.filament_id,
            preset.name,
            name_before_at(preset.name),
            *nozzle_temp_low,
            *nozzle_temp_high,
            *filament_type
        };
    }

    std::ostringstream csv;
    bool first_line = true;

    for (const auto& entry : rows) {
        const CustomFilamentRow& row = entry.second;
        if (!first_line)
            csv << '\n';
        first_line = false;

        csv << row.filament_id << ','
            << csv_escape(row.display_name) << ','
            << row.nozzle_temp_low << ','
            << row.nozzle_temp_high << ','
            << row.filament_type;
    }

    return csv.str();
}

size_t write_callback(void* data, size_t size, size_t count, void* user)
{
    auto* body = static_cast<std::string*>(user);
    body->append(static_cast<const char*>(data), size * count);
    return size * count;
}

std::string internal_api_url(const ConsoleConfig& config, const char* path)
{
    std::string address = config.address;
    while (!address.empty() && address.back() == '/')
        address.pop_back();
    return "https://" + address + path;
}

void set_common_curl_options(CURL* curl, const ConsoleConfig& config, const std::string& url, std::string& response, std::array<char, CURL_ERROR_SIZE>& curl_error)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error.data());
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
}

std::string curl_error_string(CURLcode curl_code, const std::array<char, CURL_ERROR_SIZE>& curl_error)
{
    const char* begin = curl_error.data();
    const char* end = std::find(begin, begin + curl_error.size(), '\0');
    std::string error(begin, end);
    if (error.empty())
        error = curl_easy_strerror(curl_code);
    return error;
}

PostResult post_custom_filaments(const ConsoleConfig& config, const std::string& custom_filaments)
{
    PostResult result;

    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize HTTP client.";
        return result;
    }

    const std::string url = internal_api_url(config, "/api/internal/filaments-config");
    const std::string payload = nlohmann::json{{"custom_filaments", custom_filaments}}.dump();

    std::array<char, CURL_ERROR_SIZE> curl_error{};
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + config.api_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    set_common_curl_options(curl, config, url, result.response, curl_error);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));

    const CURLcode curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_code != CURLE_OK) {
        result.error = curl_error_string(curl_code, curl_error);
        SPOOLEASE_LOG(warning) << "SpoolEase: custom filament sync failed: url=" << url
                               << " curl_code=" << static_cast<int>(curl_code)
                               << " error=\"" << result.error << "\"";
        return result;
    }

    if (result.status < 200 || result.status >= 300) {
        result.error = "SpoolEase returned HTTP " + std::to_string(result.status);
        if (!result.response.empty())
            result.error += ": " + result.response;
        SPOOLEASE_LOG(warning) << "SpoolEase: custom filament sync returned non-2xx: url=" << url
                               << " status=" << result.status
                               << " response_body_bytes=" << result.response.size();
        return result;
    }

    result.ok = true;
    SPOOLEASE_LOG(info) << "SpoolEase: custom filament sync completed: url=" << url
                        << " request_body_bytes=" << payload.size()
                        << " response_body_bytes=" << result.response.size();
    return result;
}

PostResult post_custom_filaments(const std::string& custom_filaments)
{
    PostResult result;

    const std::optional<ConsoleConfig> config = console_config(true, "sync_custom_filaments");
    if (!config.has_value()) {
        result.error = "SpoolEase is not configured.";
        return result;
    }

    return post_custom_filaments(*config, custom_filaments);
}

FetchResult fetch_custom_filaments(const ConsoleConfig& config)
{
    FetchResult result;

    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize HTTP client.";
        return result;
    }

    const std::string url = internal_api_url(config, "/api/internal/filaments-config");

    std::array<char, CURL_ERROR_SIZE> curl_error{};
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + config.api_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");

    set_common_curl_options(curl, config, url, result.response, curl_error);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_code != CURLE_OK) {
        result.error = curl_error_string(curl_code, curl_error);
        SPOOLEASE_LOG(warning) << "SpoolEase: custom filament fetch failed: url=" << url
                               << " curl_code=" << static_cast<int>(curl_code)
                               << " error=\"" << result.error << "\"";
        return result;
    }

    if (result.status < 200 || result.status >= 300) {
        result.error = "SpoolEase returned HTTP " + std::to_string(result.status);
        if (!result.response.empty())
            result.error += ": " + result.response;
        SPOOLEASE_LOG(warning) << "SpoolEase: custom filament fetch returned non-2xx: url=" << url
                               << " status=" << result.status
                               << " response_body_bytes=" << result.response.size();
        return result;
    }

    try {
        const nlohmann::json body = nlohmann::json::parse(result.response);
        const auto it = body.find("custom_filaments");
        if (it != body.end() && !it->is_null()) {
            if (!it->is_string()) {
                result.error = "SpoolEase returned invalid custom_filaments.";
                return result;
            }
            result.custom_filaments = normalize_custom_filaments_text(it->get<std::string>());
        }
    } catch (const std::exception& e) {
        result.error = std::string("Failed to parse SpoolEase response: ") + e.what();
        return result;
    }

    result.ok = true;
    SPOOLEASE_LOG(info) << "SpoolEase: custom filament fetch completed: url=" << url
                        << " response_body_bytes=" << result.response.size();
    return result;
}

class CustomFilamentAutoSync : public wxEvtHandler
{
public:
    CustomFilamentAutoSync()
        : m_timer(this)
    {
        Bind(wxEVT_TIMER, &CustomFilamentAutoSync::on_timer, this);
    }

    void start()
    {
        if (m_started)
            return;
        m_started = true;
        m_timer.Start(10000, wxTIMER_ONE_SHOT);
        SPOOLEASE_LOG(info) << "SpoolEase: custom filament auto-sync watcher started";
    }

private:
    void on_timer(wxTimerEvent&)
    {
        poll();
        m_timer.Start(5000, wxTIMER_ONE_SHOT);
    }

    void poll()
    {
        const std::optional<ConsoleConfig> config = console_config(false, "custom_filaments_auto_sync");
        if (!config.has_value() || !config->auto_sync_custom_filaments) {
            m_candidate_csv.clear();
            m_candidate_confirmed = false;
            return;
        }

        const std::string config_key = config->address + "\n" + config->api_token;
        if (config_key != m_config_key) {
            m_config_key = config_key;
            m_have_remote_baseline = false;
            m_last_synced_csv.clear();
            clear_failure_state();
        }

        const std::string csv = normalize_custom_filaments_text(build_custom_filaments_csv());
        if (csv.empty()) {
            m_candidate_csv.clear();
            m_candidate_confirmed = false;
            return;
        }

        if (csv != m_candidate_csv) {
            m_candidate_csv = csv;
            m_candidate_confirmed = false;
            return;
        }

        if (!m_candidate_confirmed)
            m_candidate_confirmed = true;

        if (m_in_flight)
            return;
        if (m_have_remote_baseline && csv == m_last_synced_csv)
            return;

        if (m_failure_csv != csv)
            clear_failure_state();

        const auto now = std::chrono::steady_clock::now();
        if (m_consecutive_failures >= 2 && now < m_next_retry)
            return;

        start_worker(*config, csv, !m_have_remote_baseline);
    }

    void start_worker(ConsoleConfig config, std::string csv, bool compare_remote)
    {
        m_in_flight = true;
        std::thread([this, config = std::move(config), csv = std::move(csv), compare_remote]() {
            bool ok = false;
            bool posted = false;
            std::string error;

            if (compare_remote) {
                const FetchResult fetch = fetch_custom_filaments(config);
                if (!fetch.ok) {
                    error = fetch.error;
                } else if (fetch.custom_filaments == csv) {
                    ok = true;
                } else {
                    const PostResult post = post_custom_filaments(config, csv);
                    ok = post.ok;
                    posted = post.ok;
                    error = post.error;
                }
            } else {
                const PostResult post = post_custom_filaments(config, csv);
                ok = post.ok;
                posted = post.ok;
                error = post.error;
            }

            if (wxTheApp) {
                wxTheApp->CallAfter([this, csv = std::move(csv), ok, posted, error = std::move(error)]() {
                    on_worker_finished(csv, ok, posted, error);
                });
            }
        }).detach();
    }

    void on_worker_finished(const std::string& csv, bool ok, bool posted, const std::string& error)
    {
        m_in_flight = false;

        if (ok) {
            m_have_remote_baseline = true;
            m_last_synced_csv = csv;
            clear_failure_state();
            if (posted)
                request_web_page_reload();
            return;
        }

        if (m_failure_csv != csv) {
            m_failure_csv = csv;
            m_consecutive_failures = 0;
        }
        ++m_consecutive_failures;
        if (m_consecutive_failures >= 2)
            m_next_retry = std::chrono::steady_clock::now() + std::chrono::seconds(60);

        SPOOLEASE_LOG(warning) << "SpoolEase: custom filament auto-sync failed: failure_count=" << m_consecutive_failures
                               << " error=\"" << error << "\"";
    }

    void clear_failure_state()
    {
        m_failure_csv.clear();
        m_consecutive_failures = 0;
        m_next_retry = std::chrono::steady_clock::time_point{};
    }

private:
    wxTimer m_timer;
    bool    m_started{false};
    bool    m_candidate_confirmed{false};
    bool    m_have_remote_baseline{false};
    bool    m_in_flight{false};
    int     m_consecutive_failures{0};
    std::chrono::steady_clock::time_point m_next_retry{};
    std::string m_config_key;
    std::string m_candidate_csv;
    std::string m_last_synced_csv;
    std::string m_failure_csv;
};

class SyncCustomFilamentsDialog : public Slic3r::GUI::DPIDialog
{
public:
    SyncCustomFilamentsDialog(wxWindow* parent, const std::string& csv)
        : Slic3r::GUI::DPIDialog(parent, wxID_ANY, _L("Sync Custom Filaments"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* intro = new wxStaticText(this, wxID_ANY, _L("Review and edit the custom filament CSV that will be submitted to SpoolEase."));
        intro->Wrap(FromDIP(620));
        sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

        m_text = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(csv), wxDefaultPosition, wxSize(FromDIP(640), FromDIP(340)), wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL | wxVSCROLL);
        wxFont text_font = m_text->GetFont();
        text_font.SetFamily(wxFONTFAMILY_TELETYPE);
        m_text->SetFont(text_font);
        sizer->Add(m_text, 1, wxEXPAND | wxALL, FromDIP(16));

        m_count = new wxStaticText(this, wxID_ANY, wxEmptyString);
        sizer->Add(m_count, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT, FromDIP(16));

        m_warning = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_warning->SetForegroundColour(*wxRED);
        m_warning->Wrap(FromDIP(620));
        sizer->Add(m_warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        auto* button_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        m_ok = new wxButton(this, wxID_OK, _L("OK"));
        button_sizer->AddStretchSpacer();
        button_sizer->Add(cancel, 0, wxRIGHT, FromDIP(8));
        button_sizer->Add(m_ok, 0);
        sizer->Add(button_sizer, 0, wxEXPAND | wxALL, FromDIP(16));

        SetSizer(sizer);
        SetMinSize(wxSize(FromDIP(680), FromDIP(520)));
        SetSize(wxSize(FromDIP(700), FromDIP(600)));
        SetEscapeId(wxID_CANCEL);
        SetAffirmativeId(wxID_OK);
        CenterOnParent();

        m_text->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_validity(); });
        update_validity();
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
    }

    std::string custom_filaments() const { return to_utf8(m_text->GetValue()); }

private:
    void on_dpi_changed(const wxRect&) override {}

    void update_validity()
    {
        const wxString text = m_text->GetValue();
        const size_t count = text.length();
        const bool empty = trim_copy(to_utf8(text)).empty();
        const bool too_long = count > custom_filaments_max_chars;

        if (too_long) {
            m_count->SetLabel(wxString::Format(_L("%llu / %llu characters (exceeds by %llu)"),
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(custom_filaments_max_chars),
                static_cast<unsigned long long>(count - custom_filaments_max_chars)));
            m_warning->SetLabel(_L("This exceeds what SpoolEase can accept. Remove unneeded lines or other content to reduce the text to 4000 characters."));
        } else {
            m_count->SetLabel(wxString::Format(_L("%llu / %llu characters (%llu available)"),
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(custom_filaments_max_chars),
                static_cast<unsigned long long>(custom_filaments_max_chars - count)));
            m_warning->SetLabel(empty ? wxString(_L("There is no custom filament text to sync.")) : wxString());
        }

        m_ok->Enable(!empty && !too_long);
        Layout();
    }

private:
    wxTextCtrl*  m_text{nullptr};
    wxStaticText* m_count{nullptr};
    wxStaticText* m_warning{nullptr};
    wxButton*   m_ok{nullptr};
};

} // namespace

void sync_custom_filaments(wxWindow& parent)
{
    const std::string csv = build_custom_filaments_csv();
    if (trim_copy(csv).empty()) {
        wxMessageBox(_L("No custom filaments were found to sync."), _L("Sync Custom Filaments"), wxOK | wxICON_INFORMATION, &parent);
        return;
    }

    SyncCustomFilamentsDialog dialog(&parent, csv);
    if (dialog.ShowModal() != wxID_OK)
        return;

    const std::string custom_filaments = dialog.custom_filaments();

    const PostResult result = post_custom_filaments(custom_filaments);
    if (!result.ok) {
        wxMessageBox(wxString::Format(_L("Failed to sync custom filaments:\n%s"), wxString::FromUTF8(result.error)), _L("Sync Custom Filaments"), wxOK | wxICON_ERROR, &parent);
        return;
    }

    request_web_page_reload();

    wxMessageBox(_L("Custom filaments synced successfully."), _L("Sync Custom Filaments"), wxOK | wxICON_INFORMATION, &parent);
}

void start_custom_filament_auto_sync()
{
    static CustomFilamentAutoSync auto_sync;
    auto_sync.start();
}

}} // namespace Slic3r::SpoolEase
