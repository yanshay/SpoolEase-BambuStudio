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

namespace {

namespace fs = boost::filesystem;

std::string config_path()
{
    if (Slic3r::data_dir().empty())
        return {};
    return (fs::path(Slic3r::data_dir()) / "spoolease" / "config.json").string();
}

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

std::optional<ConsoleConfig> console_config(bool warn)
{
    const std::string path = config_path();

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

            return ConsoleConfig{address, security_key, config_string(section, "api_token"), config_string(section, "ca_cert_pem")};
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

std::string filament_manager_url()
{
    start_inventory_polling();

    const std::optional<ConsoleConfig> config = console_config(true);
    if (!config.has_value())
        return {};

    return "http://" + config->address + "/app/inventory#sk=" + config->security_key;
}

}} // namespace Slic3r::SpoolEase
