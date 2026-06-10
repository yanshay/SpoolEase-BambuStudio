#include "SpoolEaseConfig.hpp"

#include "SpoolEaseInventory.hpp"
#include "SpoolEaseLog.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/msgdlg.h>

#include <exception>
#include <mutex>
#include <utility>

namespace Slic3r {
const std::string& data_dir();
}

namespace Slic3r { namespace SpoolEase {

wxDEFINE_EVENT(EVT_SPOOLEASE_CONFIG_CHANGED, wxCommandEvent);

namespace fs = boost::filesystem;

namespace {

struct ConfigReadResult
{
    std::optional<ConsoleConfig> config;
    std::string                  path;
    std::string                  status;
    std::string                  reason;
    std::string                  missing_field;
    std::string                  warning_message;
};

struct RuntimeConfigState
{
    bool                         loaded{false};
    std::optional<ConsoleConfig> config;
    std::string                  path;
    std::string                  status;
    std::string                  reason;
    std::string                  missing_field;
    std::string                  warning_message;
    bool                         warning_shown{false};
};

std::mutex         s_runtime_config_mutex;
RuntimeConfigState s_runtime_config;

const char* source_name(const char* source)
{
    return source && *source ? source : "runtime";
}

const char* bool_text(bool value)
{
    return value ? "true" : "false";
}

void show_config_warning(const std::string& message)
{
    wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8("SpoolEase configuration"), wxOK | wxICON_WARNING);
}

void notify_config_changed()
{
    refresh_inventory_now();

    if (!wxTheApp) {
        SPOOLEASE_LOG(warning) << "SpoolEase: config change notification failed: wxTheApp unavailable";
        return;
    }

    wxCommandEvent event(EVT_SPOOLEASE_CONFIG_CHANGED);
    wxPostEvent(wxTheApp, event);
    SPOOLEASE_LOG(info) << "SpoolEase: config change event posted; API refresh requested";
}

std::string config_string(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_string())
        return {};
    return it->get<std::string>();
}

void log_config_path_resolved_once(const std::string& path)
{
    if (path.empty())
        return;

    static std::mutex mutex;
    static bool logged = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (logged)
        return;

    logged = true;
    SPOOLEASE_LOG(info) << "SpoolEase: config path resolved: path=" << path;
}

void log_config_read_result(const char* source, const ConfigReadResult& result)
{
    const char* safe_source = source_name(source);
    if (result.status == "loaded") {
        const ConsoleConfig& config = *result.config;
        SPOOLEASE_LOG(info) << "SpoolEase: config loaded: source=" << safe_source
                            << " path=" << result.path
                            << " address=" << config.address
                            << " api_token_present=" << bool_text(!config.api_token.empty())
                            << " security_key_present=" << bool_text(!config.security_key.empty())
                            << " ca_cert_present=" << bool_text(!config.ca_cert_pem.empty());
    } else if (result.status == "not_found") {
        SPOOLEASE_LOG(info) << "SpoolEase: config not found: source=" << safe_source << " path=" << result.path;
    } else if (result.status == "read_failed") {
        SPOOLEASE_LOG(error) << "SpoolEase: config read failed: source=" << safe_source
                             << " path=" << result.path
                             << " error=\"" << result.reason << "\"";
    } else if (!result.missing_field.empty()) {
        SPOOLEASE_LOG(warning) << "SpoolEase: config missing required field: source=" << safe_source
                               << " path=" << result.path
                               << " field=" << result.missing_field;
    } else {
        SPOOLEASE_LOG(warning) << "SpoolEase: config invalid: source=" << safe_source
                               << " path=" << result.path
                               << " reason=\"" << result.reason << "\"";
    }
}

ConfigReadResult invalid_result(std::string path, std::string reason, std::string warning_message)
{
    ConfigReadResult result;
    result.path = std::move(path);
    result.status = "invalid";
    result.reason = std::move(reason);
    result.warning_message = std::move(warning_message);
    return result;
}

ConfigReadResult read_console_config_file(bool validate_required)
{
    ConfigReadResult result;
    result.path = config_file_path();

    try {
        if (result.path.empty()) {
            result.status = "read_failed";
            result.reason = "configuration path is empty";
            result.warning_message = "Failed to read SpoolEase configuration file.";
            return result;
        }

        if (!fs::exists(result.path)) {
            result.status = "not_found";
            return result;
        }

        boost::nowide::ifstream ifs(result.path);
        if (!ifs) {
            result.status = "read_failed";
            result.reason = "failed to open configuration file";
            result.warning_message = "Failed to read SpoolEase configuration file:\n" + result.path;
            return result;
        }

        nlohmann::json config;
        ifs >> config;

        if (!config.is_object())
            return invalid_result(result.path, "configuration file is not a JSON object", "SpoolEase configuration file is not a JSON object:\n" + result.path);

        const nlohmann::json section = config.value("console", nlohmann::json::object());
        if (!section.is_object())
            return invalid_result(result.path, "missing or invalid console object", "SpoolEase configuration is missing object 'console':\n" + result.path);

        ConsoleConfig console{
            config_string(section, "address"),
            config_string(section, "security_key"),
            config_string(section, "api_token"),
            config_string(section, "ca_cert_pem"),
            config_string(section, "backup_folder"),
            section.value("auto_sync_custom_filaments", false),
            section.value("auto_backup_enabled", false),
            std::max(1, section.value("auto_backup_keep_count", 7))
        };

        if (validate_required) {
            if (console.address.empty())
                result.missing_field = "console.address";
            else if (console.security_key.empty())
                result.missing_field = "console.security_key";
            else if (console.api_token.empty())
                result.missing_field = "console.api_token";

            if (!result.missing_field.empty()) {
                result.status = "invalid";
                result.reason = "missing required field " + result.missing_field;
                result.warning_message = "SpoolEase configuration is missing '" + result.missing_field + "':\n" + result.path;
                return result;
            }
        }

        result.status = "loaded";
        result.config = std::move(console);
        return result;
    } catch (const std::exception& e) {
        result.status = "read_failed";
        result.reason = e.what();
        result.warning_message = "Failed to read SpoolEase configuration file:\n" + result.path + "\n\n" + e.what();
    } catch (...) {
        result.status = "read_failed";
        result.reason = "unknown error";
        result.warning_message = "Failed to read SpoolEase configuration file:\n" + result.path;
    }

    return result;
}

void store_runtime_result(const ConfigReadResult& result)
{
    s_runtime_config.loaded = true;
    s_runtime_config.config = result.config;
    s_runtime_config.path = result.path;
    s_runtime_config.status = result.status;
    s_runtime_config.reason = result.reason;
    s_runtime_config.missing_field = result.missing_field;
    s_runtime_config.warning_message = result.warning_message;
    s_runtime_config.warning_shown = false;
}

void update_runtime_cache_after_save(const ConsoleConfig& console, const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_runtime_config_mutex);
    s_runtime_config.loaded = true;
    s_runtime_config.config = console;
    s_runtime_config.path = path;
    s_runtime_config.status = "loaded";
    s_runtime_config.reason.clear();
    s_runtime_config.missing_field.clear();
    s_runtime_config.warning_message.clear();
    s_runtime_config.warning_shown = false;
}

void clear_runtime_cache_after_erase(const std::string& path)
{
    std::lock_guard<std::mutex> lock(s_runtime_config_mutex);
    s_runtime_config.loaded = true;
    s_runtime_config.config.reset();
    s_runtime_config.path = path;
    s_runtime_config.status = "not_found";
    s_runtime_config.reason.clear();
    s_runtime_config.missing_field.clear();
    s_runtime_config.warning_message.clear();
    s_runtime_config.warning_shown = false;
}

bool fail_save(const std::string& path, const std::string& message, std::string* error)
{
    if (error)
        *error = message;
    SPOOLEASE_LOG(error) << "SpoolEase: config save failed: path=" << path << " error=\"" << message << "\"";
    return false;
}

bool fail_erase(const std::string& path, const std::string& message, std::string* error)
{
    if (error)
        *error = message;
    SPOOLEASE_LOG(error) << "SpoolEase: config erase failed: path=" << path << " error=\"" << message << "\"";
    return false;
}

} // namespace

std::string config_file_path()
{
    if (Slic3r::data_dir().empty())
        return {};
    const std::string path = (fs::path(Slic3r::data_dir()) / "spoolease" / "config.json").string();
    log_config_path_resolved_once(path);
    return path;
}

std::optional<ConsoleConfig> console_config(bool warn, const char* source)
{
    std::optional<ConsoleConfig> config;
    std::string warning_message;

    {
        std::lock_guard<std::mutex> lock(s_runtime_config_mutex);
        if (!s_runtime_config.loaded) {
            const ConfigReadResult result = read_console_config_file(true);
            store_runtime_result(result);
            log_config_read_result(source, result);
        }

        config = s_runtime_config.config;
        if (warn && !config.has_value() && !s_runtime_config.warning_shown && !s_runtime_config.warning_message.empty()) {
            s_runtime_config.warning_shown = true;
            warning_message = s_runtime_config.warning_message;
        }
    }

    if (!warning_message.empty())
        show_config_warning(warning_message);

    return config;
}

std::optional<ConsoleConfig> console_config_for_edit(std::string* error)
{
    if (error)
        error->clear();

    const ConfigReadResult result = read_console_config_file(false);
    log_config_read_result("settings_dialog", result);

    if (result.status == "loaded")
        return result.config;

    if (error && result.status != "not_found")
        *error = result.reason.empty() ? "Unknown error." : result.reason;

    return std::nullopt;
}

bool save_console_config(const ConsoleConfig& console, std::string* error)
{
    if (error)
        error->clear();

    const std::string path = config_file_path();
    try {
        if (path.empty())
            return fail_save(path, "SpoolEase configuration path is empty.", error);

        fs::create_directories(fs::path(path).parent_path());

        nlohmann::json config;
        config["version"] = 1;
        config["console"] = {
            {"address", console.address},
            {"security_key", console.security_key},
            {"api_token", console.api_token},
            {"ca_cert_pem", console.ca_cert_pem},
            {"backup_folder", console.backup_folder},
            {"auto_sync_custom_filaments", console.auto_sync_custom_filaments},
            {"auto_backup_enabled", console.auto_backup_enabled},
            {"auto_backup_keep_count", std::max(1, console.auto_backup_keep_count)}
        };

        boost::nowide::ofstream ofs(path);
        if (!ofs)
            return fail_save(path, "Failed to open configuration file for writing.", error);

        ofs << config.dump(4) << "\n";
        ofs.close();
        if (!ofs)
            return fail_save(path, "Failed to write configuration file.", error);

        SPOOLEASE_LOG(info) << "SpoolEase: config saved: path=" << path
                            << " address=" << console.address
                            << " ca_cert_present=" << bool_text(!console.ca_cert_pem.empty());
        update_runtime_cache_after_save(console, path);
        SPOOLEASE_LOG(info) << "SpoolEase: config cache updated: source=settings_save"
                            << " address=" << console.address
                            << " ca_cert_present=" << bool_text(!console.ca_cert_pem.empty());
        notify_config_changed();
        return true;
    } catch (const std::exception& e) {
        return fail_save(path, e.what(), error);
    } catch (...) {
        return fail_save(path, "Unknown error.", error);
    }
}

bool delete_console_config(std::string* error)
{
    if (error)
        error->clear();

    const std::string path = config_file_path();
    try {
        if (!path.empty() && fs::exists(path))
            fs::remove(path);

        SPOOLEASE_LOG(info) << "SpoolEase: config erased: path=" << path;
        clear_runtime_cache_after_erase(path);
        SPOOLEASE_LOG(info) << "SpoolEase: config cache cleared: source=settings_erase";
        notify_config_changed();
        return true;
    } catch (const std::exception& e) {
        return fail_erase(path, e.what(), error);
    } catch (...) {
        return fail_erase(path, "Unknown error.", error);
    }
}

std::string web_page_url()
{
    const std::optional<ConsoleConfig> config = console_config(true, "web_page");
    if (!config.has_value())
        return {};

    return "http://" + config->address + "/app/inventory#sk=" + config->security_key;
}

}} // namespace Slic3r::SpoolEase
