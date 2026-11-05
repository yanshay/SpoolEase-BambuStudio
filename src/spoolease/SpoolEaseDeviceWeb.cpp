#include "SpoolEaseDeviceWeb.hpp"

#include "SpoolEaseInventory.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceWeb/DeviceWebHost.hpp"

#include <algorithm>
#include <vector>
#include <wx/weakref.h>

namespace Slic3r { namespace SpoolEase {

namespace {
std::vector<wxWeakRef<GUI::DeviceWebHost>> s_ams_control_hosts;

void enrich_slot(nlohmann::json& slot, const std::string& printer_serial)
{
    if (!slot.is_object()) return;
    const auto slot_state = slot.value("slot_state", std::string());
    if (slot_state == "none" || slot_state == "empty") return;
    const auto inventory = slot_inventory(printer_serial, slot.value("ams_id", std::string()), slot.value("slot_id", std::string()));
    if (!inventory) return;

    slot["spoolease"] = {{"spool_id", inventory->spool_id},
                         {"short_id", display_spool_id(inventory->spool_id)},
                         {"weight", display_weight(inventory->weight_net)}};
}
}

nlohmann::json enrich_ams_control_state(nlohmann::json state, MachineObject* machine)
{
    if (!machine || !machine->GetNozzleRack() || machine->GetNozzleRack()->IsDirectConnectedRack()) return state;

    auto& area = state["display"]["ams_ext_area"];
    if (area.contains("units") && area["units"].is_array()) {
        for (auto& unit : area["units"]) {
            if (!unit.is_object() || !unit.contains("slots") || !unit["slots"].is_array()) continue;
            for (auto& slot : unit["slots"]) enrich_slot(slot, machine->get_dev_id());
        }
    }
    if (area.contains("ext_slots") && area["ext_slots"].is_array())
        for (auto& slot : area["ext_slots"]) enrich_slot(slot, machine->get_dev_id());
    return state;
}

void register_ams_control_web_host(GUI::DeviceWebHost* host)
{
    if (!host) return;
    s_ams_control_hosts.emplace_back(host);
    start_inventory_polling();
}

void refresh_ams_control_web_hosts()
{
    s_ams_control_hosts.erase(std::remove_if(s_ams_control_hosts.begin(), s_ams_control_hosts.end(),
        [](const auto& host) { return !host.get(); }), s_ams_control_hosts.end());
    for (const auto& host : s_ams_control_hosts)
        if (host.get() && host->IsShownOnScreen()) host->NotifyAmsControlWebChanged();
}

}}
