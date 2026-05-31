#ifndef spoolEaseConfig_hpp_
#define spoolEaseConfig_hpp_

#include <optional>
#include <string>

namespace Slic3r { namespace SpoolEase {

struct ConsoleConfig
{
    std::string address;
    std::string security_key;
    std::string api_token;
    std::string ca_cert_pem;
};

std::optional<ConsoleConfig> console_config(bool warn);
std::string filament_manager_url();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseConfig_hpp_
