#ifndef spoolEaseDropdownTooltip_hpp_
#define spoolEaseDropdownTooltip_hpp_

class wxString;
class wxWindow;

namespace Slic3r { namespace SpoolEase {

void show_dropdown_tooltip(wxWindow& owner, const wxString& text);
void show_ams_filament_dropdown_tooltip(wxWindow& owner, wxWindow* combo, int item_id);
void hide_dropdown_tooltip(wxWindow* owner = nullptr);

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseDropdownTooltip_hpp_
