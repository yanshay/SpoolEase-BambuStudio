#ifndef spoolEaseDeviceSlotUi_hpp_
#define spoolEaseDeviceSlotUi_hpp_

#include <string>

class wxDC;
class wxString;
class wxWindow;

namespace Slic3r { namespace SpoolEase {

void draw_device_slot_overlay(wxDC& dc, wxWindow& slot, const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id);
void update_device_slot_tooltip(wxWindow& slot, wxString& tooltip, const wxString& material_name, const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id);

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseDeviceSlotUi_hpp_
