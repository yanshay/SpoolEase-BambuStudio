#include "SpoolEaseFilamentTooltips.hpp"

#include "SpoolEaseInventory.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"

#include <wx/weakref.h>

#include <algorithm>
#include <boost/algorithm/string/predicate.hpp>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace SpoolEase {

namespace {

struct SlotSelection
{
    std::string printer_serial;
    std::string ams_id;
    std::string slot_id;
    std::string match_label;
};

struct ItemTooltipTarget
{
    wxWeakRef<GUI::PresetComboBox> combo;
    int                            item_id{-1};
    SlotSelection                  slot;
};

std::unordered_map<int, SlotSelection> s_selected_filament_slots;
std::vector<wxWeakRef<GUI::PlaterPresetComboBox>> s_filament_combos;
std::vector<ItemTooltipTarget> s_item_tooltip_targets;

std::string current_printer_serial()
{
    auto* manager = GUI::wxGetApp().getDeviceManager();
    auto* machine = manager ? manager->get_selected_machine() : nullptr;
    return machine ? machine->get_dev_id() : std::string();
}

std::optional<int> parse_int(const std::string& value)
{
    try {
        size_t end = 0;
        int result = std::stoi(value, &end);
        if (end == value.size())
            return result;
    } catch (...) {
    }
    return std::nullopt;
}

std::string slot_label(const std::string& ams_id, const std::string& slot_id)
{
    const std::optional<int> ams = parse_int(ams_id);
    const std::optional<int> slot = parse_int(slot_id);
    if (!ams.has_value())
        return {};

    if (*ams == 255)
        return "Ext-R";
    if (*ams == 254)
        return "Ext-L";
    if (*ams >= 0 && *ams < 26 && slot.has_value())
        return std::string(1, static_cast<char>('A' + *ams)) + std::string(1, static_cast<char>('1' + *slot));
    if (*ams >= 128 && *ams < 153)
        return "HT-" + std::string(1, static_cast<char>('A' + (*ams - 128)));

    return {};
}

std::optional<SlotSelection> slot_selection_from_tray(const DynamicPrintConfig& tray, const std::string& printer_serial)
{
    const std::string ams_id = tray.opt_string("ams_id", 0u);
    const std::string slot_id = tray.opt_string("slot_id", 0u);
    const std::string tray_name = tray.opt_string("tray_name", 0u);
    if (printer_serial.empty() || ams_id.empty() || slot_id.empty())
        return std::nullopt;

    const std::string label = tray_name.empty() ? slot_label(ams_id, slot_id) : tray_name;
    return SlotSelection{printer_serial, ams_id, slot_id, label};
}

const Preset* preset_for_tray(const DynamicPrintConfig& tray)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle)
        return nullptr;

    const std::string filament_id = tray.opt_string("filament_id", 0u);
    const auto& filaments = bundle->filaments;
    const auto& presets = filaments.get_presets();
    auto it = std::find_if(presets.begin(), presets.end(), [&filaments, &filament_id](const Preset& preset) {
        return preset.is_compatible && filaments.get_preset_base(preset) == &preset && preset.filament_id == filament_id;
    });
    if (it != presets.end())
        return &*it;

    std::string filament_type = tray.opt_string("filament_type", 0u);
    if (filament_type.empty())
        return nullptr;

    filament_type = "Generic " + filament_type;
    it = std::find_if(presets.begin(), presets.end(), [&filament_type](const Preset& preset) {
        return preset.is_compatible && preset.is_system && boost::algorithm::starts_with(preset.name, filament_type);
    });
    return it == presets.end() ? nullptr : &*it;
}

std::optional<std::string> filament_color_for_combo(const GUI::PlaterPresetComboBox& combo)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    if (!bundle)
        return std::nullopt;

    const int filament_idx = combo.get_filament_idx();
    const auto* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || filament_idx < 0 || static_cast<size_t>(filament_idx) >= colors->values.size())
        return std::nullopt;

    return colors->values[filament_idx];
}

std::optional<SlotSelection> slot_selection_from_matching_ams(GUI::PlaterPresetComboBox& combo)
{
    const int selection = combo.GetSelection();
    if (selection < 0 || combo.GetFlag(selection) != static_cast<int>(GUI::PresetComboBox::FilamentAMSType::FROM_AMS))
        return std::nullopt;

    const std::optional<std::string> selected_color = filament_color_for_combo(combo);
    if (!selected_color.has_value())
        return std::nullopt;

    const wxString selected_name = combo.GetString(selection);
    const std::string printer_serial = current_printer_serial();
    for (const auto& entry : GUI::wxGetApp().preset_bundle->filament_ams_list) {
        const DynamicPrintConfig& tray = entry.second;
        if (tray.opt_string("filament_colour", 0u) != *selected_color)
            continue;

        const Preset* preset = preset_for_tray(tray);
        if (!preset || combo.get_preset_name(*preset) != selected_name)
            continue;

        std::optional<SlotSelection> slot = slot_selection_from_tray(tray, printer_serial);
        if (slot.has_value())
            return slot;
    }

    return std::nullopt;
}

std::optional<SlotSelection> slot_selection_from_combo(GUI::PlaterPresetComboBox& combo)
{
    const int selection = combo.GetSelection();
    if (selection < 0 || combo.GetFlag(selection) != static_cast<int>(GUI::PresetComboBox::FilamentAMSType::FROM_AMS))
        return std::nullopt;

    const int ams_index = combo.selected_ams_filament();
    const auto& ams_list = GUI::wxGetApp().preset_bundle->filament_ams_list;
    const auto it = ams_list.find(ams_index);
    if (it == ams_list.end())
        return slot_selection_from_matching_ams(combo);

    return slot_selection_from_tray(it->second, current_printer_serial());
}

std::optional<SlotInventory> inventory_for_slot(const SlotSelection& slot)
{
    start_inventory_polling();
    return slot_inventory(slot.printer_serial, slot.ams_id, slot.slot_id);
}

wxString details_tooltip_for_inventory(const SlotInventory& inventory)
{
    wxString tooltip;
    const std::string details = display_spool_details(inventory);
    if (!details.empty())
        tooltip += wxString::FromUTF8(details);
    if (!tooltip.empty())
        tooltip += "\n";
    tooltip += "Net Weight: ";
    tooltip += wxString::FromUTF8(display_weight(inventory.weight_net));
    return tooltip;
}

wxString selected_tooltip_section(const SlotSelection& slot, const SlotInventory& inventory)
{
    wxString tooltip = "Matches: ";
    tooltip += slot.match_label.empty() ? wxString("-") : wxString::FromUTF8(slot.match_label);
    tooltip += "\n";
    tooltip += details_tooltip_for_inventory(inventory);
    return tooltip;
}

wxString strip_selected_tooltip_section(const wxString& tooltip)
{
    const wxString marker = "\n\nMatches: ";
    int pos = tooltip.Find(marker);
    if (pos != wxNOT_FOUND)
        return tooltip.Left(pos);

    if (tooltip.StartsWith("Matches: "))
        return wxString();

    const wxString old_header = wxString::FromUTF8("--------------------\n     SpoolEase\n--------------------\n");
    pos = tooltip.Find(old_header);
    if (pos == wxNOT_FOUND)
        return tooltip;
    if (pos > 0 && tooltip.GetChar(pos - 1) == '\n')
        --pos;
    return tooltip.Left(pos);
}

void remember_filament_slot(int filament_idx, const SlotSelection& slot)
{
    if (filament_idx < 0)
        return;
    s_selected_filament_slots[filament_idx] = slot;
}

void remember_filament_combo(GUI::PlaterPresetComboBox& combo)
{
    s_filament_combos.erase(std::remove_if(s_filament_combos.begin(), s_filament_combos.end(), [](const auto& ref) { return ref.get() == nullptr; }),
                            s_filament_combos.end());
    const auto it = std::find_if(s_filament_combos.begin(), s_filament_combos.end(), [&combo](const auto& ref) { return ref.get() == &combo; });
    if (it == s_filament_combos.end())
        s_filament_combos.emplace_back(&combo);
}

void remember_item_tooltip(GUI::PresetComboBox& combo, int item_id, const SlotSelection& slot)
{
    s_item_tooltip_targets.erase(std::remove_if(s_item_tooltip_targets.begin(), s_item_tooltip_targets.end(), [](const auto& target) {
                                     return target.combo.get() == nullptr || target.item_id < 0;
                                 }),
                                 s_item_tooltip_targets.end());
    const auto it = std::find_if(s_item_tooltip_targets.begin(), s_item_tooltip_targets.end(), [&combo, item_id](const auto& target) {
        return target.combo.get() == &combo && target.item_id == item_id;
    });
    if (it == s_item_tooltip_targets.end())
        s_item_tooltip_targets.push_back(ItemTooltipTarget{wxWeakRef<GUI::PresetComboBox>(&combo), item_id, slot});
    else
        it->slot = slot;
}

void refresh_item_tooltips()
{
    s_item_tooltip_targets.erase(std::remove_if(s_item_tooltip_targets.begin(), s_item_tooltip_targets.end(), [](auto& target) {
                                     GUI::PresetComboBox* combo = target.combo.get();
                                     if (!combo || target.item_id < 0)
                                         return true;
                                     if (static_cast<unsigned int>(target.item_id) >= combo->GetCount())
                                         return true;
                                     if (combo->GetFlag(target.item_id) != static_cast<int>(GUI::PresetComboBox::FilamentAMSType::FROM_AMS)) {
                                         combo->SetItemTooltip(target.item_id, wxString());
                                         return true;
                                     }
                                     if (target.slot.printer_serial != current_printer_serial()) {
                                         combo->SetItemTooltip(target.item_id, wxString());
                                         return true;
                                     }

                                     const std::optional<SlotInventory> inventory = inventory_for_slot(target.slot);
                                     combo->SetItemTooltip(target.item_id, inventory.has_value() ? details_tooltip_for_inventory(*inventory) : wxString());
                                     return false;
                                 }),
                                 s_item_tooltip_targets.end());
}

} // namespace

void set_ams_filament_item_tooltip(GUI::PresetComboBox& combo, int item_id, const DynamicPrintConfig& tray)
{
    const std::optional<SlotSelection> slot = slot_selection_from_tray(tray, current_printer_serial());
    if (!slot.has_value())
        return;

    remember_item_tooltip(combo, item_id, *slot);
    const std::optional<SlotInventory> inventory = inventory_for_slot(*slot);
    const wxString tooltip = inventory.has_value() ? details_tooltip_for_inventory(*inventory) : wxString();
    if (!tooltip.empty())
        combo.SetItemTooltip(item_id, tooltip);
}

void record_filament_combo_selection(GUI::PlaterPresetComboBox& combo, int selection)
{
    if (combo.get_type() != Preset::TYPE_FILAMENT || selection < 0) {
        return;
    }

    const int filament_idx = combo.get_filament_idx();
    if (combo.GetFlag(selection) != static_cast<int>(GUI::PresetComboBox::FilamentAMSType::FROM_AMS)) {
        s_selected_filament_slots.erase(filament_idx);
        update_filament_combo_tooltip(combo);
        return;
    }

    const int ams_index = combo.selected_ams_filament();
    const auto& ams_list = GUI::wxGetApp().preset_bundle->filament_ams_list;
    const auto it = ams_list.find(ams_index);
    if (it == ams_list.end()) {
        s_selected_filament_slots.erase(filament_idx);
        update_filament_combo_tooltip(combo);
        return;
    }

    const std::optional<SlotSelection> slot = slot_selection_from_tray(it->second, current_printer_serial());
    if (!slot.has_value()) {
        s_selected_filament_slots.erase(filament_idx);
        update_filament_combo_tooltip(combo);
        return;
    }

    remember_filament_slot(filament_idx, *slot);
    update_filament_combo_tooltip(combo);
}

void record_filament_sync_result(const std::string& printer_serial, bool direct_sync, const std::map<int, AMSMapInfo>& sync_maps, bool skip_ext)
{
    s_selected_filament_slots.clear();
    if (printer_serial.empty())
        return;

    if (!direct_sync) {
        for (const auto& item : sync_maps) {
            const std::string& ams_id = item.second.ams_id;
            const std::string& slot_id = item.second.slot_id;
            if (!ams_id.empty() && !slot_id.empty())
                remember_filament_slot(item.first, SlotSelection{printer_serial, ams_id, slot_id, slot_label(ams_id, slot_id)});
        }
        return;
    }

    int filament_idx = 0;
    std::set<std::pair<std::string, std::string>> added_filaments;
    for (const auto& entry : GUI::wxGetApp().preset_bundle->filament_ams_list) {
        const DynamicPrintConfig& tray = entry.second;
        const std::string filament_id = tray.opt_string("filament_id", 0u);
        const std::string tray_name = tray.opt_string("tray_name", 0u);
        if (filament_id.empty() || (tray_name == "Ext" && skip_ext))
            continue;

        if (skip_ext) {
            const auto filament_pair = std::make_pair(tray_name, filament_id);
            if (added_filaments.find(filament_pair) != added_filaments.end())
                continue;
            added_filaments.insert(filament_pair);
        }

        const std::optional<SlotSelection> slot = slot_selection_from_tray(tray, printer_serial);
        if (slot.has_value())
            remember_filament_slot(filament_idx, *slot);
        ++filament_idx;
    }
}

void update_filament_combo_tooltip(GUI::PlaterPresetComboBox& combo)
{
    if (combo.get_type() != Preset::TYPE_FILAMENT)
        return;

    remember_filament_combo(combo);

    const int filament_idx = combo.get_filament_idx();
    const wxString current = combo.GetToolTipText();
    const wxString base_tooltip = strip_selected_tooltip_section(current);
    const auto set_base_tooltip = [&combo, &current, &base_tooltip]() {
        if (current != base_tooltip)
            combo.SetToolTip(base_tooltip);
    };

    std::optional<SlotSelection> slot = slot_selection_from_combo(combo);
    if (slot.has_value())
        remember_filament_slot(filament_idx, *slot);

    const auto it = s_selected_filament_slots.find(filament_idx);
    if (it == s_selected_filament_slots.end()) {
        set_base_tooltip();
        return;
    }
    if (it->second.printer_serial != current_printer_serial()) {
        set_base_tooltip();
        return;
    }

    const std::optional<SlotInventory> inventory = inventory_for_slot(it->second);
    if (!inventory.has_value()) {
        set_base_tooltip();
        return;
    }

    wxString tooltip = base_tooltip;
    if (!tooltip.empty())
        tooltip += "\n\n";
    tooltip += selected_tooltip_section(it->second, *inventory);
    if (tooltip != current)
        combo.SetToolTip(tooltip);
}

void refresh_filament_combo_tooltips()
{
    refresh_item_tooltips();

    std::vector<GUI::PlaterPresetComboBox*> combos;
    s_filament_combos.erase(std::remove_if(s_filament_combos.begin(), s_filament_combos.end(), [&combos](const auto& ref) {
                                GUI::PlaterPresetComboBox* combo = ref.get();
                                if (!combo)
                                    return true;
                                combos.push_back(combo);
                                return false;
                            }),
                            s_filament_combos.end());

    for (GUI::PlaterPresetComboBox* combo : combos)
        update_filament_combo_tooltip(*combo);
}

}} // namespace Slic3r::SpoolEase
