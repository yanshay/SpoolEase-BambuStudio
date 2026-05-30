#include "SpoolEaseDeviceSlotUi.hpp"

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/pen.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>

namespace Slic3r { namespace SpoolEase {

namespace {

wxString display_text(const std::string& text)
{
    return text.empty() ? wxString("-") : wxString::FromUTF8(text);
}

void draw_centered_text(wxDC& dc, const wxString& text, int x, int y, int width, int height)
{
    const wxSize text_size = dc.GetTextExtent(text);
    dc.DrawText(text, x + (width - text_size.x) / 2, y + (height - text_size.y) / 2);
}

} // namespace

void draw_device_slot_overlay(wxDC& dc, wxWindow& slot, const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
{
    (void) printer_serial;

    const wxSize size = slot.GetClientSize();
    if (size.x <= 0 || size.y <= 0)
        return;

    const wxColour material_text_colour = dc.GetTextForeground();

    auto dip = [&slot](int value) { return slot.FromDIP(value); };

    wxFont pill_font = slot.GetFont();
    if (!pill_font.IsOk())
        pill_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    pill_font.SetPointSize(std::max(9, pill_font.GetPointSize()));
    pill_font.SetWeight(wxFONTWEIGHT_BOLD);

    wxFont weight_font = pill_font;
    weight_font.SetPointSize(std::max(9, pill_font.GetPointSize() - 1));

    const int margin = dip(5);
    const int pill_height = std::min(dip(18), std::max(dip(14), size.y / 5 + dip(2)));
    const int pill_width = std::max(dip(24), size.x - 2 * margin);
    const int pill_x = (size.x - pill_width) / 2;
    const int pill_y = dip(1);

    dc.SetPen(wxPen(wxColour(0, 174, 66), dip(1)));
    dc.SetBrush(wxBrush(wxColour(206, 245, 218)));
    dc.DrawRoundedRectangle(pill_x, pill_y, pill_width, pill_height, pill_height / 2);

    dc.SetFont(pill_font);
    dc.SetTextForeground(wxColour(0, 0, 0));
    draw_centered_text(dc, display_text(ams_id), pill_x, pill_y - dip(1), pill_width, pill_height);

    dc.SetFont(weight_font);
    dc.SetTextForeground(material_text_colour);
    draw_centered_text(dc, display_text(slot_id), 0, pill_y + pill_height + dip(3) - 1, size.x, dip(14));
}

}} // namespace Slic3r::SpoolEase
