#include "SpoolEaseConfig.hpp"

#include "SpoolEaseInventory.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/msgdlg.h>

#include <exception>

namespace Slic3r {
const std::string& data_dir();
}

namespace Slic3r { namespace SpoolEase {

namespace fs = boost::filesystem;

namespace {

void show_config_warning_once(const std::string& message)
{
    static bool shown = false;
    if (shown)
        return;
    shown = true;
    wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8("SpoolEase configuration"), wxOK | wxICON_WARNING);
}

std::string config_string(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    if (it == object.end() || it->is_null() || !it->is_string())
        return {};
    return it->get<std::string>();
}

} // namespace

std::string config_file_path()
{
    if (Slic3r::data_dir().empty())
        return {};
    return (fs::path(Slic3r::data_dir()) / "spoolease" / "config.json").string();
}

std::optional<ConsoleConfig> console_config(bool warn)
{
    const std::string path = config_file_path();

    try {
        if (!path.empty() && fs::exists(path)) {
            boost::nowide::ifstream ifs(path);
            nlohmann::json config;
            ifs >> config;

            if (!config.is_object()) {
                if (warn)
                    show_config_warning_once("SpoolEase configuration file is not a JSON object:\n" + path);
                return std::nullopt;
            }

            const nlohmann::json section = config.value("console", nlohmann::json::object());
            if (!section.is_object()) {
                if (warn)
                    show_config_warning_once("SpoolEase configuration is missing object 'console':\n" + path);
                return std::nullopt;
            }

            std::string address = config_string(section, "address");
            if (address.empty()) {
                if (warn)
                    show_config_warning_once("SpoolEase configuration is missing 'console.address':\n" + path);
                return std::nullopt;
            }

            std::string security_key = config_string(section, "security_key");
            if (security_key.empty()) {
                if (warn)
                    show_config_warning_once("SpoolEase configuration is missing 'console.security_key':\n" + path);
                return std::nullopt;
            }

            std::string api_token = config_string(section, "api_token");
            if (api_token.empty()) {
                if (warn)
                    show_config_warning_once("SpoolEase configuration is missing 'console.api_token':\n" + path);
                return std::nullopt;
            }

            return ConsoleConfig{address, security_key, api_token, config_string(section, "ca_cert_pem")};
        }
    } catch (const std::exception& e) {
        if (warn)
            show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path + "\n\n" + e.what());
    } catch (...) {
        if (warn)
            show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path);
    }

    return std::nullopt;
}

std::optional<ConsoleConfig> console_config_for_edit(std::string* error)
{
    if (error)
        error->clear();

    const std::string path = config_file_path();
    try {
        if (path.empty() || !fs::exists(path))
            return std::nullopt;

        boost::nowide::ifstream ifs(path);
        nlohmann::json config;
        ifs >> config;
        if (!config.is_object()) {
            if (error)
                *error = "SpoolEase configuration file is not a JSON object.";
            return std::nullopt;
        }

        const nlohmann::json section = config.value("console", nlohmann::json::object());
        if (!section.is_object()) {
            if (error)
                *error = "SpoolEase configuration is missing object 'console'.";
            return std::nullopt;
        }

        return ConsoleConfig{
            config_string(section, "address"),
            config_string(section, "security_key"),
            config_string(section, "api_token"),
            config_string(section, "ca_cert_pem")
        };
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
    } catch (...) {
        if (error)
            *error = "Unknown error.";
    }

    return std::nullopt;
}

bool save_console_config(const ConsoleConfig& console, std::string* error)
{
    if (error)
        error->clear();

    const std::string path = config_file_path();
    try {
        if (path.empty()) {
            if (error)
                *error = "SpoolEase configuration path is empty.";
            return false;
        }

        fs::create_directories(fs::path(path).parent_path());

        nlohmann::json config;
        config["version"] = 1;
        config["console"] = {
            {"address", console.address},
            {"security_key", console.security_key},
            {"api_token", console.api_token},
            {"ca_cert_pem", console.ca_cert_pem}
        };

        boost::nowide::ofstream ofs(path);
        if (!ofs) {
            if (error)
                *error = "Failed to open configuration file for writing.";
            return false;
        }

        ofs << config.dump(4) << "\n";
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
    } catch (...) {
        if (error)
            *error = "Unknown error.";
    }

    return false;
}

bool delete_console_config(std::string* error)
{
    if (error)
        error->clear();

    const std::string path = config_file_path();
    try {
        if (path.empty())
            return true;
        if (fs::exists(path))
            fs::remove(path);
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
    } catch (...) {
        if (error)
            *error = "Unknown error.";
    }

    return false;
}

std::string filament_manager_url()
{
    start_inventory_polling();

    const std::optional<ConsoleConfig> config = console_config(true);
    if (!config.has_value())
        return {};

    return "http://" + config->address + "/app/inventory#sk=" + config->security_key;
}

}} // namespace Slic3r::SpoolEase
