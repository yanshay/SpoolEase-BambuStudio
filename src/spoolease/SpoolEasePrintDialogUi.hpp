#ifndef spoolEasePrintDialogUi_hpp_
#define spoolEasePrintDialogUi_hpp_

#include <optional>

#include <wx/gdicmn.h>

class wxDC;
class wxColour;
class wxFont;
class wxString;
class wxWindow;

namespace Slic3r { namespace SpoolEase {

wxSize  print_dialog_item_size(const wxWindow& item, const wxSize& bambu_size);
wxSize  print_dialog_bambu_size(const wxWindow& item, const wxSize& current_size);
void    adjust_print_dialog_item_size(wxWindow& item);
wxPoint begin_print_dialog_item_render(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxColour& border_colour, int border_width, int corner_radius);
void    end_print_dialog_item_render(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxPoint& original_origin, const wxString& slot_label, const wxFont& font, wxColour text_colour, const wxColour& border_colour, int border_width, int corner_radius);
void    set_print_dialog_required_weight(wxWindow& item, std::optional<float> required_g);

template<class Filaments>
void set_print_dialog_required_weight_from_filaments(wxWindow& item, const Filaments& filaments, int filament_id)
{
    for (const auto& filament : filaments) {
        if (filament.id == filament_id) {
            set_print_dialog_required_weight(item, filament.used_g > 0.f ? std::optional<float>(filament.used_g) : std::nullopt);
            return;
        }
    }

    set_print_dialog_required_weight(item, std::nullopt);
}

template<class GCodeResult, class DensitiesOption>
void set_print_dialog_required_weight_from_gcode(wxWindow& item, const GCodeResult* gcode_result, const DensitiesOption* filament_densities, int filament_id)
{
    if (!gcode_result || !filament_densities || filament_id < 0 || filament_id >= (int) filament_densities->values.size()) {
        set_print_dialog_required_weight(item, std::nullopt);
        return;
    }

    const auto& volumes = gcode_result->print_statistics.total_volumes_per_extruder;
    auto volume_it = volumes.find(filament_id);
    if (volume_it == volumes.end()) {
        set_print_dialog_required_weight(item, std::nullopt);
        return;
    }

    set_print_dialog_required_weight(item, filament_densities->values[filament_id] * (volume_it->second / 1000.f));
}

}} // namespace Slic3r::SpoolEase

#endif // spoolEasePrintDialogUi_hpp_
