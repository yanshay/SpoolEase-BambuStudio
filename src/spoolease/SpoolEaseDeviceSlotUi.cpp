#include "SpoolEaseDeviceSlotUi.hpp"

#include "SpoolEaseInventory.hpp"

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
#include <unordered_map>

namespace Slic3r { namespace SpoolEase {

namespace {

enum class SlotOverlayLayout
{
    Normal,
    Compact,
    WeightOnly,
};

std::unordered_map<wxWindow*, SlotOverlayLayout> s_slot_layout;

wxString display_text(const std::string& text)
{
    return text.empty() ? wxString("-") : wxString::FromUTF8(text);
}

void draw_centered_text(wxDC& dc, const wxString& text, int x, int y, int width, int height)
{
    const wxSize text_size = dc.GetTextExtent(text);
    dc.DrawText(text, x + (width - text_size.x) / 2, y + (height - text_size.y) / 2);
}

SlotOverlayLayout bambu_slot_overlay_layout(const wxString& tooltip, const wxString& material_name)
{
    const bool material_splits = material_name.Find(' ') != wxNOT_FOUND || material_name.Find('-') != wxNOT_FOUND;
    const bool has_k_line = tooltip.Find('\n') != wxNOT_FOUND;

    if (material_splits)
        return SlotOverlayLayout::WeightOnly;
    if (has_k_line)
        return SlotOverlayLayout::Compact;
    return SlotOverlayLayout::Normal;
}

SlotOverlayLayout slot_overlay_layout(wxWindow& slot)
{
    auto it = s_slot_layout.find(&slot);
    return it != s_slot_layout.end() ? it->second : SlotOverlayLayout::Normal;
}

} // namespace

void draw_device_slot_overlay(wxDC& dc, wxWindow& slot, const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
{
    const std::optional<SlotInventory> inventory = slot_inventory(printer_serial, ams_id, slot_id);
    if (!inventory.has_value())
        return;

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
    const SlotOverlayLayout layout = slot_overlay_layout(slot);
    const bool compact = layout == SlotOverlayLayout::Compact;
    const bool weight_only = layout == SlotOverlayLayout::WeightOnly;
    const int overlay_pill_height = compact ? std::max(dip(1), pill_height - dip(5)) : pill_height;
    const int normal_weight_y = pill_y + pill_height + dip(3) - 1;

    if (!weight_only) {
        dc.SetPen(wxPen(wxColour(0, 174, 66), dip(1)));
        dc.SetBrush(wxBrush(wxColour(206, 245, 218)));
        dc.DrawRoundedRectangle(pill_x, pill_y, pill_width, overlay_pill_height, overlay_pill_height / 2);

        dc.SetFont(pill_font);
        dc.SetTextForeground(wxColour(0, 0, 0));
        draw_centered_text(dc, display_text(display_spool_id(inventory->spool_id)), pill_x, pill_y - dip(1), pill_width, overlay_pill_height);
    }

    dc.SetFont(weight_font);
    dc.SetTextForeground(material_text_colour);
    draw_centered_text(dc, display_text(display_weight(inventory->weight_net)), 0, weight_only ? pill_y : compact ? normal_weight_y - dip(9) : normal_weight_y, size.x, dip(14));
}

void update_device_slot_tooltip(wxWindow& slot, wxString& tooltip, const wxString& material_name, const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id)
{
    s_slot_layout[&slot] = bambu_slot_overlay_layout(tooltip, material_name);

    const std::optional<SlotInventory> inventory = slot_inventory(printer_serial, ams_id, slot_id);
    if (!inventory.has_value())
        return;

    if (!tooltip.empty())
        tooltip += "\n--------------------\n";
        tooltip +=   "     SpoolEase\n";
        tooltip +=   "--------------------\n";

    tooltip += "Spool ID: ";
    tooltip += inventory->spool_id.empty() ? wxString("-") : wxString::FromUTF8(inventory->spool_id);
    tooltip += "\nNet weight: ";
    tooltip += wxString::FromUTF8(display_weight(inventory->weight_net));
}

}} // namespace Slic3r::SpoolEase
