#pragma once

#include <nlohmann/json.hpp>

namespace Slic3r {
class MachineObject;
namespace GUI { class DeviceWebHost; }
namespace SpoolEase {

nlohmann::json enrich_ams_control_state(nlohmann::json state, MachineObject* machine);
void register_ams_control_web_host(GUI::DeviceWebHost* host);
void refresh_ams_control_web_hosts();

}}
