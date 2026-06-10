#include "SpoolEaseBackup.hpp"

#include "SpoolEaseConfig.hpp"
#include "SpoolEaseLog.hpp"
#include "SpoolEaseStatus.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace SpoolEase {

namespace fs = boost::filesystem;

namespace {

std::once_flag s_curl_init_once;
std::atomic_bool s_backup_operation_in_progress{false};

constexpr const char* automatic_backup_prefix = "SpoolEase-AutoBackup-";
constexpr const char* backup_extension = ".txt";
constexpr int automatic_backup_startup_delay_ms = 2 * 60 * 1000;
constexpr int automatic_backup_retry_delay_ms = 60 * 60 * 1000;
constexpr int automatic_backup_busy_retry_delay_ms = 10 * 60 * 1000;

struct DownloadResult
{
    bool        ok{false};
    bool        cancelled{false};
    long        status{0};
    std::string error;
    std::string data;
};

struct PostResult
{
    bool        ok{false};
    long        status{0};
    std::string error;
    std::string response;
};

struct AutoBackupResult
{
    bool        ok{false};
    std::string error;
    std::string path;
    std::string warning;
};

struct AutomaticBackupFile
{
    std::string name;
    fs::path    path;
};

struct CurlTransferState
{
    std::atomic_bool* cancel{nullptr};
};

std::string to_utf8(const wxString& text)
{
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer.data() ? std::string(buffer.data()) : std::string();
}

bool try_begin_backup_operation()
{
    bool expected = false;
    return s_backup_operation_in_progress.compare_exchange_strong(expected, true);
}

void end_backup_operation()
{
    s_backup_operation_in_progress.store(false);
}

std::tm local_time(std::time_t value)
{
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &value);
#else
    localtime_r(&value, &local);
#endif
    return local;
}

std::string format_local_time(std::time_t value, const char* format)
{
    std::tm local = local_time(value);
    std::ostringstream out;
    out << std::put_time(&local, format);
    return out.str();
}

std::string internal_api_url(const ConsoleConfig& config, const char* path)
{
    std::string address = config.address;
    while (!address.empty() && address.back() == '/')
        address.pop_back();
    return "https://" + address + path;
}

size_t write_callback(void* data, size_t size, size_t count, void* user)
{
    auto* body = static_cast<std::string*>(user);
    body->append(static_cast<const char*>(data), size * count);
    return size * count;
}

int transfer_callback(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto* state = static_cast<CurlTransferState*>(user);
    return state && state->cancel && state->cancel->load() ? 1 : 0;
}

int transfer_callback_legacy(void* user, double, double, double, double)
{
    const auto* state = static_cast<CurlTransferState*>(user);
    return state && state->cancel && state->cancel->load() ? 1 : 0;
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

void set_common_curl_options(CURL* curl, const ConsoleConfig& config, const std::string& url, std::string& response, std::array<char, CURL_ERROR_SIZE>& curl_error, long total_timeout_seconds)
{
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error.data());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    if (total_timeout_seconds > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_timeout_seconds);
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

DownloadResult download_backup_to_memory(const ConsoleConfig& config, std::atomic_bool& cancel)
{
    DownloadResult result;
    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize HTTP client.";
        return result;
    }

    const std::string url = internal_api_url(config, "/api/internal/store-backup");
    std::array<char, CURL_ERROR_SIZE> curl_error{};
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + config.api_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Accept: text/plain");

    CurlTransferState transfer_state{&cancel};
    set_common_curl_options(curl, config, url, result.data, curl_error, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer_state);
#else
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, transfer_callback_legacy);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &transfer_state);
#endif

    const CURLcode curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_code == CURLE_ABORTED_BY_CALLBACK && cancel.load()) {
        result.cancelled = true;
        result.error = "Backup cancelled.";
        SPOOLEASE_LOG(info) << "SpoolEase: backup download cancelled: url=" << url;
        return result;
    }

    if (curl_code != CURLE_OK) {
        result.error = curl_error_string(curl_code, curl_error);
        SPOOLEASE_LOG(warning) << "SpoolEase: backup download failed: url=" << url
                               << " curl_code=" << static_cast<int>(curl_code)
                               << " error=\"" << result.error << "\"";
        return result;
    }

    if (result.status < 200 || result.status >= 300) {
        result.error = "API returned HTTP " + std::to_string(result.status);
        if (!result.data.empty())
            result.error += ": " + result.data;
        SPOOLEASE_LOG(warning) << "SpoolEase: backup download returned non-2xx: url=" << url
                               << " status=" << result.status
                               << " response_body_bytes=" << result.data.size();
        return result;
    }

    result.ok = true;
    SPOOLEASE_LOG(info) << "SpoolEase: backup download completed: url=" << url
                        << " response_body_bytes=" << result.data.size();
    return result;
}

PostResult mark_backup_completed(const ConsoleConfig& config, std::time_t completed_at)
{
    PostResult result;
    std::call_once(s_curl_init_once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize HTTP client.";
        return result;
    }

    const std::string url = internal_api_url(config, "/api/internal/store-backup/mark-completed");
    const std::string payload = nlohmann::json{{"date_time", static_cast<long long>(completed_at)}}.dump();

    std::array<char, CURL_ERROR_SIZE> curl_error{};
    struct curl_slist* headers = nullptr;
    const std::string auth_header = "Authorization: Bearer " + config.api_token;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");

    set_common_curl_options(curl, config, url, result.response, curl_error, 4L);
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
        SPOOLEASE_LOG(warning) << "SpoolEase: backup completion mark failed: url=" << url
                               << " curl_code=" << static_cast<int>(curl_code)
                               << " error=\"" << result.error << "\"";
        return result;
    }

    if (result.status < 200 || result.status >= 300) {
        result.error = "API returned HTTP " + std::to_string(result.status);
        if (!result.response.empty())
            result.error += ": " + result.response;
        SPOOLEASE_LOG(warning) << "SpoolEase: backup completion mark returned non-2xx: url=" << url
                               << " status=" << result.status
                               << " response_body_bytes=" << result.response.size();
        return result;
    }

    result.ok = true;
    return result;
}

bool is_existing_directory(const std::string& path)
{
    boost::system::error_code ec;
    return !path.empty() && fs::exists(path, ec) && !ec && fs::is_directory(path, ec) && !ec;
}

std::string default_backup_filename()
{
    const std::time_t now = std::time(nullptr);
    return "SpoolEase-Backup-" + format_local_time(now, "%Y%m%d-%H%M%S") + backup_extension;
}

std::string automatic_backup_filename(std::time_t value)
{
    return std::string(automatic_backup_prefix) + format_local_time(value, "%Y%m%d-%H%M%S") + backup_extension;
}

std::string local_date_stamp(std::time_t value)
{
    return format_local_time(value, "%Y%m%d");
}

std::string join_path(const std::string& folder, const std::string& filename)
{
    return (fs::path(folder) / filename).string();
}

void remove_file_silent(const std::string& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

bool all_digits(const std::string& text, size_t pos, size_t count)
{
    if (pos + count > text.size())
        return false;
    for (size_t i = pos; i < pos + count; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return false;
    }
    return true;
}

bool is_automatic_backup_filename(const std::string& name)
{
    const std::string prefix(automatic_backup_prefix);
    const std::string extension(backup_extension);
    const size_t expected_size = prefix.size() + 8 + 1 + 6 + extension.size();
    return name.size() == expected_size
        && name.compare(0, prefix.size(), prefix) == 0
        && name[prefix.size() + 8] == '-'
        && name.compare(name.size() - extension.size(), extension.size(), extension) == 0
        && all_digits(name, prefix.size(), 8)
        && all_digits(name, prefix.size() + 9, 6);
}

std::string automatic_backup_date_from_filename(const std::string& name)
{
    return name.substr(std::string(automatic_backup_prefix).size(), 8);
}

bool non_empty_regular_file(const fs::path& path)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec)
        return false;
    const auto size = fs::file_size(path, ec);
    return !ec && size > 0;
}

bool automatic_backup_exists_for_date(const std::string& folder, const std::string& date_stamp)
{
    if (!is_existing_directory(folder))
        return false;

    try {
        for (fs::directory_iterator it(folder), end; it != end; ++it) {
            const fs::path path = it->path();
            const std::string name = path.filename().string();
            if (is_automatic_backup_filename(name)
                && automatic_backup_date_from_filename(name) == date_stamp
                && non_empty_regular_file(path))
                return true;
        }
    } catch (const std::exception& e) {
        SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup scan failed: folder=" << folder
                               << " error=\"" << e.what() << "\"";
    }

    return false;
}

std::string prune_automatic_backups(const std::string& folder, int keep_count)
{
    keep_count = std::max(1, keep_count);
    if (!is_existing_directory(folder))
        return {};

    std::vector<AutomaticBackupFile> files;

    try {
        for (fs::directory_iterator it(folder), end; it != end; ++it) {
            const fs::path path = it->path();
            const std::string name = path.filename().string();
            if (!is_automatic_backup_filename(name))
                continue;

            boost::system::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec)
                continue;

            const auto size = fs::file_size(path, ec);
            if (ec)
                continue;

            if (size == 0) {
                fs::remove(path, ec);
                if (ec) {
                    SPOOLEASE_LOG(warning) << "SpoolEase: empty automatic backup cleanup failed: path=" << path.string()
                                           << " error=\"" << ec.message() << "\"";
                    return "Automatic backup cleanup failed: " + ec.message();
                }
                continue;
            }

            files.push_back(AutomaticBackupFile{name, path});
        }
    } catch (const std::exception& e) {
        SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup retention scan failed: folder=" << folder
                               << " error=\"" << e.what() << "\"";
        return std::string("Automatic backup retention scan failed: ") + e.what();
    }

    std::sort(files.begin(), files.end(), [](const AutomaticBackupFile& lhs, const AutomaticBackupFile& rhs) {
        return lhs.name < rhs.name;
    });

    while (files.size() > static_cast<size_t>(keep_count)) {
        boost::system::error_code ec;
        fs::remove(files.front().path, ec);
        if (ec) {
            SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup retention delete failed: path=" << files.front().path.string()
                                   << " error=\"" << ec.message() << "\"";
            return "Automatic backup retention cleanup failed: " + ec.message();
        }
        SPOOLEASE_LOG(info) << "SpoolEase: automatic backup retention deleted: path=" << files.front().path.string();
        files.erase(files.begin());
    }

    return {};
}

int milliseconds_from_seconds(long long seconds)
{
    const long long milliseconds = std::max(1LL, seconds) * 1000LL;
    return static_cast<int>(std::min<long long>(milliseconds, std::numeric_limits<int>::max()));
}

int milliseconds_until_next_daily_check()
{
    const std::time_t now = std::time(nullptr);
    std::tm next = local_time(now);
    next.tm_hour = 0;
    next.tm_min = 0;
    next.tm_sec = 0;
    next.tm_mday += 1;

    const std::time_t next_midnight = std::mktime(&next);
    long long seconds = 60;
    if (next_midnight != static_cast<std::time_t>(-1))
        seconds = static_cast<long long>(std::difftime(next_midnight, now)) + 120;
    return milliseconds_from_seconds(std::max(60LL, seconds));
}

bool write_backup_file(const std::string& path, const std::string& data, std::string& error)
{
    if (data.empty()) {
        error = "Backup response was empty.";
        return false;
    }

    boost::nowide::ofstream out(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Failed to open backup file for writing.";
        return false;
    }

    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        error = "Failed to write backup file.";
        remove_file_silent(path);
        return false;
    }

    return true;
}

AutoBackupResult run_automatic_backup(ConsoleConfig config)
{
    AutoBackupResult result;

    if (!is_existing_directory(config.backup_folder)) {
        result.error = "Automatic backup folder is not configured or does not exist.";
        return result;
    }

    std::atomic_bool cancel{false};
    DownloadResult download = download_backup_to_memory(config, cancel);
    if (!download.ok) {
        result.error = download.error.empty() ? "Backup download failed." : download.error;
        return result;
    }

    if (download.data.empty()) {
        result.error = "Backup response was empty.";
        SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup rejected empty response";
        return result;
    }

    const std::string save_path = join_path(config.backup_folder, automatic_backup_filename(std::time(nullptr)));
    std::string save_error;
    if (!write_backup_file(save_path, download.data, save_error)) {
        result.error = save_error;
        remove_file_silent(save_path);
        return result;
    }

    const PostResult mark_result = mark_backup_completed(config, std::time(nullptr));
    if (!mark_result.ok) {
        SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup saved but completion mark failed: path=" << save_path
                               << " error=\"" << mark_result.error << "\"";
        result.warning = "Automatic backup completion mark failed: " + mark_result.error;
    }

    const std::string prune_warning = prune_automatic_backups(config.backup_folder, config.auto_backup_keep_count);
    if (!prune_warning.empty()) {
        if (!result.warning.empty())
            result.warning += "\n";
        result.warning += prune_warning;
    }

    result.ok = true;
    result.path = save_path;
    SPOOLEASE_LOG(info) << "SpoolEase: automatic backup saved: path=" << save_path
                        << " bytes=" << download.data.size();
    return result;
}

wxString elapsed_label(std::chrono::steady_clock::duration elapsed)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    return wxString::Format(_L("Elapsed time: %02lld:%02lld"), static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
}

wxString backup_filename_placeholder()
{
    return _L("SpoolEase-Backup-<date>-<time>.txt");
}

wxString configured_path_placeholder(const std::string& folder)
{
    return wxString::Format(_L("Configured path: %s"), wxString::FromUTF8(join_path(folder, to_utf8(backup_filename_placeholder()))));
}

class BackupDialog : public Slic3r::GUI::DPIDialog
{
public:
    BackupDialog(wxWindow* parent, ConsoleConfig config)
        : Slic3r::GUI::DPIDialog(parent, wxID_ANY, _L("Back Up SpoolEase Store"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_config(std::move(config))
        , m_timer(this)
    {
        m_default_available = is_existing_directory(m_config.backup_folder);
        build_ui();
        Bind(wxEVT_TIMER, &BackupDialog::on_timer, this);
        Bind(wxEVT_CLOSE_WINDOW, &BackupDialog::on_close, this);
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
    }

    ~BackupDialog() override
    {
        m_cancel.store(true);
        if (m_worker.joinable())
            m_worker.join();
        if (m_backup_operation_active) {
            end_backup_operation();
            m_backup_operation_active = false;
        }
    }

private:
    void build_ui()
    {
        const int margin = FromDIP(18);
        const int label_indent = FromDIP(27);
        const int content_width = FromDIP(560);
        const int detail_width = content_width - label_indent;

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto add_indented = [sizer, margin, label_indent](wxWindow* window, int top) {
            if (top > 0)
                sizer->AddSpacer(top);
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->AddSpacer(label_indent);
            row->Add(window, 1, wxEXPAND);
            sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);
        };

        auto* intro = new wxStaticText(this, wxID_ANY, _L("Save a local backup of the SpoolEase store to this computer."));
        intro->Wrap(content_width);
        sizer->Add(intro, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

        m_use_default = new wxRadioButton(this, wxID_ANY, _L("Save to configured backup folder"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        m_use_default->Enable(m_default_available);
        m_use_default->SetValue(m_default_available);
        sizer->Add(m_use_default, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

        m_default_file_label = new wxStaticText(this, wxID_ANY, m_default_available ? configured_path_placeholder(m_config.backup_folder) : wxString(_L("No backup folder is configured. Choose a save location instead.")));
        m_default_file_label->Wrap(detail_width);
        m_default_file_label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));
        add_indented(m_default_file_label, FromDIP(6));

        m_choose_location = new wxRadioButton(this, wxID_ANY, _L("Choose save location after download"));
        m_choose_location->SetValue(!m_default_available);
        sizer->AddSpacer(FromDIP(14));
        sizer->Add(m_choose_location, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

        m_choose_location_hint = new wxStaticText(this, wxID_ANY, _L("You will be asked where to save the backup file."));
        m_choose_location_hint->Wrap(detail_width);
        m_choose_location_hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        m_choose_location_hint->SetMinSize(wxSize(detail_width, FromDIP(20)));
        add_indented(m_choose_location_hint, FromDIP(3));

        m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_status->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        m_status->SetMinSize(wxSize(detail_width, FromDIP(22)));
        add_indented(m_status, FromDIP(18));

        m_elapsed = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_elapsed->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        m_elapsed->SetMinSize(wxSize(detail_width, FromDIP(20)));
        add_indented(m_elapsed, FromDIP(6));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_cancel_button = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        m_backup_button = new wxButton(this, wxID_OK, _L("Back Up"));
        buttons->AddStretchSpacer();
        buttons->Add(m_cancel_button, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_backup_button, 0);
        sizer->AddSpacer(FromDIP(12));
        sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, margin);

        SetSizerAndFit(sizer);
        SetMinSize(GetSize());
        CenterOnParent();

        m_backup_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_backup(); });
        m_cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel_or_close(); });
    }

    void start_backup()
    {
        if (m_running)
            return;

        if (m_worker.joinable())
            m_worker.join();

        if (!try_begin_backup_operation()) {
            set_status(_L("Another SpoolEase backup is already running."), true);
            return;
        }
        m_backup_operation_active = true;

        m_running = true;
        m_cancel.store(false);
        m_started = std::chrono::steady_clock::now();
        m_backup_button->Enable(false);
        m_use_default->Enable(false);
        m_choose_location->Enable(false);
        m_cancel_button->SetLabel(_L("Cancel"));
        set_status(_L("Downloading backup..."), false);
        m_elapsed->SetLabel(elapsed_label(std::chrono::seconds(0)));
        m_timer.Start(1000);

        m_worker = std::thread([this]() {
            DownloadResult result = download_backup_to_memory(m_config, m_cancel);
            if (wxTheApp) {
                wxTheApp->CallAfter([this, result = std::move(result)]() mutable { on_download_complete(std::move(result)); });
            }
        });
    }

    void on_download_complete(DownloadResult result)
    {
        if (m_worker.joinable())
            m_worker.join();
        m_timer.Stop();
        m_elapsed->SetLabel(elapsed_label(std::chrono::steady_clock::now() - m_started));

        if (result.cancelled) {
            finish(_L("Backup cancelled."), true, false);
            return;
        }

        if (!result.ok) {
            finish(_L("Backup failed."), true, false, wxString::Format(_L("Failed to back up SpoolEase:\n\n%s"), wxString::FromUTF8(result.error)));
            return;
        }

        if (result.data.empty()) {
            finish(_L("Backup failed."), true, false, _L("Backup response was empty. No backup file was saved."));
            return;
        }

        std::string save_path;
        if (m_default_available && m_use_default->GetValue()) {
            save_path = join_path(m_config.backup_folder, default_backup_filename());
            boost::system::error_code ec;
            if (fs::exists(save_path, ec) && !ec) {
                const int answer = wxMessageBox(wxString::Format(_L("The file already exists:\n%s\n\nOverwrite it?"), wxString::FromUTF8(save_path)), _L("Back Up SpoolEase Store"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this);
                if (answer != wxYES) {
                    finish(_L("Backup not saved."), true, false);
                    return;
                }
            }
        } else {
            save_path = prompt_save_path();
            if (save_path.empty()) {
                finish(_L("Backup not saved."), true, false);
                return;
            }
        }

        set_status(_L("Saving backup..."), false);
        std::string save_error;
        if (!write_backup_file(save_path, result.data, save_error)) {
            finish(_L("Failed to save backup."), true, false, wxString::Format(_L("Failed to save backup to:\n%s\n\n%s"), wxString::FromUTF8(save_path), wxString::FromUTF8(save_error)));
            return;
        }

        set_status(_L("Recording backup completion..."), false);
        const PostResult mark_result = mark_backup_completed(m_config, std::time(nullptr));
        if (!mark_result.ok) {
            finish(_L("Backup saved, but status update failed."), true, true, wxString::Format(_L("Backup was saved to:\n%s\n\nFailed to update backup status:\n%s"), wxString::FromUTF8(save_path), wxString::FromUTF8(mark_result.error)));
            return;
        }

        finish(_L("Backup saved successfully."), false, true);
    }

    std::string prompt_save_path()
    {
        const std::string default_dir = is_existing_directory(m_config.backup_folder) ? m_config.backup_folder : std::string();
        wxFileDialog dialog(this, _L("Save SpoolEase Backup"), wxString::FromUTF8(default_dir), wxString::FromUTF8(default_backup_filename()), _L("Text files (*.txt)|*.txt|All files (*.*)|*.*"), wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return {};
        return to_utf8(dialog.GetPath());
    }

    void finish(const wxString& message, bool error, bool saved, const wxString& details = wxString())
    {
        if (m_backup_operation_active) {
            end_backup_operation();
            m_backup_operation_active = false;
        }
        m_running = false;
        set_status(message, error);
        if (!details.empty())
            wxMessageBox(details, _L("Back Up SpoolEase Store"), wxOK | (error ? wxICON_ERROR : wxICON_INFORMATION), this);
        m_cancel_button->Enable(true);
        m_cancel_button->SetLabel(_L("Close"));
        m_backup_button->Enable(!saved);
        m_use_default->Enable(m_default_available && !saved);
        m_choose_location->Enable(!saved);
    }

    void cancel_or_close()
    {
        if (m_running) {
            m_cancel.store(true);
            m_cancel_button->Enable(false);
            set_status(_L("Cancelling backup..."), false);
            return;
        }

        EndModal(wxID_CANCEL);
    }

    void on_timer(wxTimerEvent&)
    {
        if (m_running)
            m_elapsed->SetLabel(elapsed_label(std::chrono::steady_clock::now() - m_started));
    }

    void on_close(wxCloseEvent& event)
    {
        if (m_running) {
            m_cancel.store(true);
            m_cancel_button->Enable(false);
            set_status(_L("Cancelling backup..."), false);
            event.Veto();
            return;
        }
        event.Skip();
    }

    void set_status(const wxString& message, bool error)
    {
        m_status->SetForegroundColour(error ? *wxRED : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
        m_status->SetLabel(message);
        m_status->Refresh();
    }

    void on_dpi_changed(const wxRect&) override {}

private:
    ConsoleConfig m_config;
    bool          m_default_available{false};
    wxRadioButton* m_use_default{nullptr};
    wxRadioButton* m_choose_location{nullptr};
    wxStaticText*  m_default_file_label{nullptr};
    wxStaticText*  m_choose_location_hint{nullptr};
    wxStaticText*  m_elapsed{nullptr};
    wxStaticText*  m_status{nullptr};
    wxButton*      m_cancel_button{nullptr};
    wxButton*      m_backup_button{nullptr};
    wxTimer        m_timer;
    std::thread    m_worker;
    std::atomic_bool m_cancel{false};
    bool          m_running{false};
    bool          m_backup_operation_active{false};
    std::chrono::steady_clock::time_point m_started;
};

class AutomaticBackupScheduler : public wxEvtHandler
{
public:
    AutomaticBackupScheduler()
        : m_timer(this)
    {
        Bind(wxEVT_TIMER, &AutomaticBackupScheduler::on_timer, this);
    }

    void start()
    {
        if (m_started)
            return;

        m_started = true;
        if (wxTheApp)
            wxTheApp->Bind(EVT_SPOOLEASE_CONFIG_CHANGED, &AutomaticBackupScheduler::on_config_changed, this);
        schedule_after(automatic_backup_startup_delay_ms);
        SPOOLEASE_LOG(info) << "SpoolEase: automatic backup scheduler started";
    }

private:
    void on_config_changed(wxCommandEvent&)
    {
        if (!m_in_flight)
            schedule_after(automatic_backup_startup_delay_ms);
    }

    void on_timer(wxTimerEvent&)
    {
        poll();
    }

    void schedule_after(int milliseconds)
    {
        m_timer.Start(std::max(1000, milliseconds), wxTIMER_ONE_SHOT);
    }

    void schedule_next_daily_check()
    {
        schedule_after(milliseconds_until_next_daily_check());
    }

    void poll()
    {
        if (m_in_flight)
            return;

        const std::optional<ConsoleConfig> config = console_config(false, "automatic_backup");
        if (!config.has_value() || !config->auto_backup_enabled) {
            clear_live_status("backup_config");
            schedule_after(automatic_backup_retry_delay_ms);
            return;
        }

        if (!is_existing_directory(config->backup_folder)) {
            SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup skipped: backup folder missing or invalid: folder=" << config->backup_folder;
            set_live_status_warning("backup_config", "Automatic backup is enabled, but the backup folder is missing or invalid: " + config->backup_folder);
            schedule_after(automatic_backup_retry_delay_ms);
            return;
        }

        clear_live_status("backup_config");

        const std::string today = local_date_stamp(std::time(nullptr));
        if (automatic_backup_exists_for_date(config->backup_folder, today)) {
            clear_sticky_status("automatic_backup");
            schedule_next_daily_check();
            return;
        }

        start_worker(*config);
    }

    void start_worker(ConsoleConfig config)
    {
        if (!try_begin_backup_operation()) {
            schedule_after(automatic_backup_busy_retry_delay_ms);
            return;
        }

        m_in_flight = true;
        std::thread([this, config = std::move(config)]() mutable {
            AutoBackupResult result = run_automatic_backup(std::move(config));
            end_backup_operation();

            if (wxTheApp) {
                wxTheApp->CallAfter([this, result = std::move(result)]() mutable {
                    on_worker_finished(std::move(result));
                });
            }
        }).detach();
    }

    void on_worker_finished(AutoBackupResult result)
    {
        m_in_flight = false;
        if (result.ok) {
            if (result.warning.empty())
                clear_sticky_status("automatic_backup");
            else
                set_sticky_status_error("automatic_backup", result.warning);
            schedule_next_daily_check();
            return;
        }

        SPOOLEASE_LOG(warning) << "SpoolEase: automatic backup failed: error=\"" << result.error << "\"";
        set_sticky_status_error("automatic_backup", "Automatic backup failed: " + result.error);
        schedule_after(automatic_backup_retry_delay_ms);
    }

private:
    wxTimer m_timer;
    bool    m_started{false};
    bool    m_in_flight{false};
};

} // namespace

void backup_to_local_disk(wxWindow& parent)
{
    const std::optional<ConsoleConfig> config = console_config(true, "backup");
    if (!config.has_value())
        return;

    BackupDialog dialog(&parent, *config);
    dialog.ShowModal();
}

void start_automatic_backup_scheduler()
{
    static AutomaticBackupScheduler scheduler;
    scheduler.start();
}

}} // namespace Slic3r::SpoolEase
