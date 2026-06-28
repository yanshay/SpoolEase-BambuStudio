#ifndef spoolEaseConfig_hpp_
#define spoolEaseConfig_hpp_

#include <optional>
#include <string>

#include <wx/event.h>

namespace Slic3r { namespace SpoolEase {

struct ConsoleConfig
{
    std::string address;
    std::string security_key;
    std::string api_token;
    std::string ca_cert_pem;
    std::string backup_folder;
    bool        auto_sync_custom_filaments{false};
    bool        auto_backup_enabled{false};
    int         auto_backup_keep_count{7};
    bool        proxy_printer_messages_configured{false};
    bool        proxy_printer_messages{false};

    bool proxy_printer_messages_enabled() const { return proxy_printer_messages_configured && proxy_printer_messages; }
};

wxDECLARE_EVENT(EVT_SPOOLEASE_CONFIG_CHANGED, wxCommandEvent);

std::string config_file_path();
std::optional<ConsoleConfig> console_config(bool warn, const char* source = nullptr);
std::optional<ConsoleConfig> console_config_for_edit(std::string* error = nullptr);
bool save_console_config(const ConsoleConfig& config, std::string* error = nullptr);
bool delete_console_config(std::string* error = nullptr);
std::string web_page_url();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseConfig_hpp_
