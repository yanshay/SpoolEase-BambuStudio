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

            const nlohmann::json section = config.value("filament_manager", nlohmann::json::object());
            if (!section.is_object()) {
                show_config_warning_once("SpoolEase configuration is missing object 'filament_manager':\n" + path);
                return {};
            }

            std::string url = section.value("url", std::string());
            if (url.empty()) {
                show_config_warning_once("SpoolEase configuration is missing 'filament_manager.url':\n" + path);
                return {};
            }

            return url;
        }
    } catch (const std::exception& e) {
        show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path + "\n\n" + e.what());
    } catch (...) {
        show_config_warning_once("Failed to read SpoolEase configuration file:\n" + path);
    }

    return {};
}

}} // namespace Slic3r::SpoolEase
