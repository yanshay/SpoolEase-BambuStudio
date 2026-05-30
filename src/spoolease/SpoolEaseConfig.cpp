#include "SpoolEaseConfig.hpp"

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

} // namespace

std::string filament_manager_url()
{
    const std::string path = config_path();

    try {
        if (!path.empty() && fs::exists(path)) {
            boost::nowide::ifstream ifs(path);
            nlohmann::json config;
            ifs >> config;

            if (!config.is_object()) {
                show_config_warning_once("SpoolEase configuration file is not a JSON object:\n" + path);
                return {};
            }

            const nlohmann::json section = config.value("console", nlohmann::json::object());
            if (!section.is_object()) {
                show_config_warning_once("SpoolEase configuration is missing object 'console':\n" + path);
                return {};
            }

            std::string address = section.value("address", std::string());
            if (address.empty()) {
                show_config_warning_once("SpoolEase configuration is missing 'console.address':\n" + path);
                return {};
            }

            std::string security_key = section.value("security_key", std::string());
            if (security_key.empty()) {
                show_config_warning_once("SpoolEase configuration is missing 'console.security_key':\n" + path);
                return {};
            }

            return "http://" + address + "/app/inventory#sk=" + security_key;
        }
    } catch (const std::exception& e) {
        show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path + "\n\n" + e.what());
    } catch (...) {
        show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path);
    }

    return {};
}

}} // namespace Slic3r::SpoolEase
