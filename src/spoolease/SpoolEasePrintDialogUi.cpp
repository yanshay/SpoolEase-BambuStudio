#include "SpoolEasePrintDialogUi.hpp"

#include "SpoolEaseInventory.hpp"

#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/pen.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/window.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>

namespace Slic3r { namespace SpoolEase {

namespace {

struct ItemState
{
    std::optional<wxSize> bambu_size;
    std::optional<float>  required_g;
    std::string           printer_serial;
    bool                  cleanup_bound{false};
};

std::unordered_map<const wxWindow*, ItemState> s_item_states;

bool is_active(const wxWindow& item)
{
    return s_item_states.find(&item) != s_item_states.end();
}

ItemState* state_for(const wxWindow& item)
{
    auto it = s_item_states.find(&item);
    return it == s_item_states.end() ? nullptr : &it->second;
}

int top_extra(const wxWindow& item)
{
    return item.FromDIP(20);
}

int bottom_extra(const wxWindow& item)
{
    return item.FromDIP(48);
}

wxSize total_size_from_bambu(const wxWindow& item, const wxSize& bambu_size)
{
    return wxSize(bambu_size.x, bambu_size.y + top_extra(item) + bottom_extra(item));
}

std::string format_weight(std::optional<float> weight)
{
    if (!weight.has_value())
        return "NO-G";

    std::ostringstream out;
    out << std::fixed << std::setprecision(weight.value() < 10.f ? 1 : 0) << weight.value() << "g";
    return out.str();
}

struct ParsedSlot
{
    std::string ams_id;
    std::string slot_id;
};

std::optional<ParsedSlot> parse_slot_label(wxString label)
{
    label.Trim(true).Trim(false);
    const std::string text = label.ToStdString();
    if (text.empty() || text == "-")
        return std::nullopt;

    if (text == "Ext" || text == "Ext-R")
        return ParsedSlot{"255", "0"};

    if (text == "Ext-L")
        return ParsedSlot{"254", "0"};

    if (text.size() == 4 && text.rfind("HT-", 0) == 0 && text[3] >= 'A' && text[3] <= 'Z')
        return ParsedSlot{std::to_string(128 + text[3] - 'A'), "0"};

    if (text.size() == 2 && text[0] >= 'A' && text[0] <= 'D' && text[1] >= '1' && text[1] <= '4')
        return ParsedSlot{std::to_string(text[0] - 'A'), std::to_string(text[1] - '1')};

    return std::nullopt;
}

wxFont base_font(const wxWindow& item, int point_size, wxFontWeight weight = wxFONTWEIGHT_NORMAL)
{
    wxFont font = item.GetFont();
    if (!font.IsOk())
        font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.SetPointSize(std::max(7, point_size));
    font.SetWeight(weight);
    return font;
}

void draw_centered_text(wxDC& dc, const wxString& text, int x, int y, int width, int height)
{
    const wxSize text_size = dc.GetTextExtent(text);
    dc.DrawText(text, x + (width - text_size.x) / 2, y + (height - text_size.y) / 2);
}

wxString visible_text(const std::string& text)
{
    return wxString::FromUTF8(text.empty() ? std::string("-") : text);
}

std::optional<SlotInventory> inventory_for_slot_label(const ItemState& state, const wxString& slot_label)
{
    const std::optional<ParsedSlot> slot = parse_slot_label(slot_label);
    if (!slot.has_value())
        return std::nullopt;

    return slot_inventory(state.printer_serial, slot->ams_id, slot->slot_id);
}

enum class CapacityStatus
{
    Normal,
    NearLimit,
    Insufficient
};

CapacityStatus capacity_status(const ItemState& state, const SlotInventory& inventory)
{
    if (!state.required_g.has_value() || !inventory.weight_net.has_value())
        return CapacityStatus::Normal;

    if (state.required_g.value() > inventory.weight_net.value())
        return CapacityStatus::Insufficient;

    if (inventory.weight_net.value() - state.required_g.value() <= 10.f)
        return CapacityStatus::NearLimit;

    return CapacityStatus::Normal;
}

wxColour capacity_background_colour(CapacityStatus status)
{
    if (status == CapacityStatus::Insufficient)
        return wxColour(230, 0, 0);
    if (status == CapacityStatus::NearLimit)
        return wxColour(255, 225, 0);
    return wxColour();
}

wxColour capacity_text_colour(CapacityStatus status, const wxColour& normal_colour)
{
    if (status == CapacityStatus::Insufficient)
        return *wxWHITE;
    if (status == CapacityStatus::NearLimit)
        return *wxBLACK;
    return normal_colour;
}

wxColour capacity_border_colour(CapacityStatus status, const wxColour& normal_colour)
{
    return status == CapacityStatus::Normal ? normal_colour : capacity_background_colour(status);
}

int capacity_border_width(const wxWindow& item, CapacityStatus status, int normal_width)
{
    return status == CapacityStatus::Normal ? normal_width : std::max(normal_width, item.FromDIP(2));
}

void draw_capacity_background(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, CapacityStatus status, int corner_radius)
{
    if (status == CapacityStatus::Normal)
        return;

    const wxSize size = item.GetClientSize();
    const int bottom_y = top_extra(item) + bambu_size.y;
    const int y = std::max(0, bottom_y);
    if (y >= size.y)
        return;

    const int height = size.y - y;
    const int radius = std::max(0, std::min(corner_radius, height / 2));

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(capacity_background_colour(status)));

    if (radius <= 0) {
        dc.DrawRectangle(0, y, size.x, height);
        return;
    }

    dc.DrawRectangle(0, y, size.x, height - radius);
    dc.DrawRoundedRectangle(0, size.y - 2 * radius, size.x, 2 * radius, radius);
}

void draw_outer_frame(wxDC& dc, const wxWindow& item, const wxColour& border_colour, int border_width, int corner_radius, bool fill)
{
    const wxSize size = item.GetClientSize();
    if (size.x <= 0 || size.y <= 0)
        return;

    dc.SetPen(wxPen(border_colour, border_width));
    dc.SetBrush(fill ? wxBrush(item.GetBackgroundColour().IsOk() ? item.GetBackgroundColour() : *wxWHITE) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, corner_radius);
}

CapacityStatus draw_extra_content(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxString& slot_label, const wxFont& font, wxColour text_colour, int corner_radius)
{
    const ItemState* state = state_for(item);
    if (!state)
        return CapacityStatus::Normal;

    const wxSize size = item.GetClientSize();
    const int top_h = top_extra(item);
    const int bottom_y = top_h + bambu_size.y;

    dc.SetFont(font.IsOk() ? font : base_font(item, 12, wxFONTWEIGHT_NORMAL));
    dc.SetTextForeground(text_colour);
    draw_centered_text(dc, wxString::FromUTF8(format_weight(state->required_g)), 0, item.FromDIP(1), size.x, top_h - item.FromDIP(2));

    const std::optional<SlotInventory> inventory = inventory_for_slot_label(*state, slot_label);
    if (!inventory.has_value())
        return CapacityStatus::Normal;

    const CapacityStatus status = capacity_status(*state, *inventory);
    draw_capacity_background(dc, item, bambu_size, status, corner_radius);

    wxFont pill_font = font.IsOk() ? font : base_font(item, 12, wxFONTWEIGHT_NORMAL);
    dc.SetFont(pill_font);
    const wxString pill_text = visible_text(display_spool_id(inventory->spool_id));
    const wxSize pill_text_size = dc.GetTextExtent(pill_text);
    const wxSize four_char_size = dc.GetTextExtent("0000");
    const int margin = item.FromDIP(5);
    const int pill_h = item.FromDIP(20);
    const int pill_w = std::min(size.x - 2 * margin, std::max(four_char_size.x + item.FromDIP(12), pill_text_size.x + item.FromDIP(12)));
    const int pill_x = (size.x - pill_w) / 2;
    const int pill_y = bottom_y + item.FromDIP(4);

    dc.SetPen(wxPen(wxColour(0, 174, 66), item.FromDIP(1)));
    dc.SetBrush(wxBrush(wxColour(206, 245, 218)));
    dc.DrawRoundedRectangle(pill_x, pill_y, pill_w, pill_h, pill_h / 2);

    dc.SetTextForeground(wxColour(0, 0, 0));
    draw_centered_text(dc, pill_text, pill_x, pill_y - item.FromDIP(1), pill_w, pill_h);

    const int slot_y = pill_y + pill_h + item.FromDIP(2) - 1;
    dc.SetFont(font.IsOk() ? font : base_font(item, 12, wxFONTWEIGHT_NORMAL));
    dc.SetTextForeground(capacity_text_colour(status, text_colour));
    draw_centered_text(dc, visible_text(display_weight(inventory->weight_net)), 0, slot_y, size.x, item.FromDIP(20));
    return status;
}

void apply_item_size(wxWindow& item)
{
    ItemState* state = state_for(item);
    if (!state)
        return;

    if (!state->bambu_size.has_value())
        state->bambu_size = item.GetSize();

    const wxSize size = total_size_from_bambu(item, *state->bambu_size);
    item.SetSize(size);
    item.SetMinSize(size);
    item.SetMaxSize(size);
}

} // namespace

wxSize print_dialog_item_size(const wxWindow& item, const wxSize& bambu_size)
{
    ItemState* state = state_for(item);
    if (!state)
        return bambu_size;

    state->bambu_size = bambu_size;
    return total_size_from_bambu(item, bambu_size);
}

wxSize print_dialog_bambu_size(const wxWindow& item, const wxSize& current_size)
{
    const ItemState* state = state_for(item);
    if (!state)
        return current_size;

    if (state->bambu_size.has_value())
        return *state->bambu_size;

    return wxSize(current_size.x, std::max(1, current_size.y - top_extra(item) - bottom_extra(item)));
}

void adjust_print_dialog_item_size(wxWindow& item)
{
    ItemState* state = state_for(item);
    if (!state)
        return;

    state->bambu_size = item.GetSize();
    apply_item_size(item);
}

wxPoint begin_print_dialog_item_render(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxColour& border_colour, int border_width, int corner_radius)
{
    wxCoord origin_x = 0;
    wxCoord origin_y = 0;
    dc.GetDeviceOrigin(&origin_x, &origin_y);

    if (!is_active(item))
        return wxPoint(origin_x, origin_y);

    draw_outer_frame(dc, item, border_colour, border_width, corner_radius, true);
    dc.SetDeviceOrigin(origin_x, origin_y + top_extra(item));
    return wxPoint(origin_x, origin_y);
}

void end_print_dialog_item_render(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxPoint& original_origin, const wxString& slot_label, const wxFont& font, wxColour text_colour, const wxColour& border_colour, int border_width, int corner_radius)
{
    if (!is_active(item))
        return;

    dc.SetDeviceOrigin(original_origin.x, original_origin.y);
    const CapacityStatus status = draw_extra_content(dc, item, bambu_size, slot_label, font, text_colour, corner_radius);
    draw_outer_frame(dc, item, capacity_border_colour(status, border_colour), capacity_border_width(item, status, border_width), corner_radius, false);
}

void set_print_dialog_required_weight(wxWindow& item, std::optional<float> required_g, const std::string& printer_serial)
{
    start_inventory_polling();

    ItemState& state = s_item_states[&item];
    state.required_g = required_g;
    state.printer_serial = printer_serial;
    if (!state.cleanup_bound) {
        item.Bind(wxEVT_DESTROY, [](wxWindowDestroyEvent& event) {
            if (auto* window = dynamic_cast<wxWindow*>(event.GetEventObject()))
                s_item_states.erase(window);
            event.Skip();
        });
        state.cleanup_bound = true;
    }
    apply_item_size(item);
    item.Refresh();
}

}} // namespace Slic3r::SpoolEase
