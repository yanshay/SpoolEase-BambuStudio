#ifndef spoolEaseFilamentTooltips_hpp_
#define spoolEaseFilamentTooltips_hpp_

#include <map>
#include <string>

#include "libslic3r/PresetBundle.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {
class PlaterPresetComboBox;
class PresetComboBox;
}

namespace SpoolEase {

void set_ams_filament_item_tooltip(GUI::PresetComboBox& combo, int item_id, const DynamicPrintConfig& tray);
void record_filament_combo_selection(GUI::PlaterPresetComboBox& combo, int selection);
void record_filament_sync_result(const std::string& printer_serial, bool direct_sync, const std::map<int, AMSMapInfo>& sync_maps, bool skip_ext);
void update_filament_combo_tooltip(GUI::PlaterPresetComboBox& combo);
void refresh_filament_combo_tooltips();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseFilamentTooltips_hpp_
