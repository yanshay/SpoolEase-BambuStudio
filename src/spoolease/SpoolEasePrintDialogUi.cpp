#include "SpoolEasePrintDialogUi.hpp"

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
    std::string ams_id{"-"};
    std::string slot_id{"-"};
};

ParsedSlot parse_slot_label(wxString label)
{
    label.Trim(true).Trim(false);
    const std::string text = label.ToStdString();
    if (text.empty() || text == "-")
        return {};

    if (text == "Ext" || text == "Ext-R" || text == "Ext-L")
        return {text, "0"};

    if (text.rfind("HT-", 0) == 0)
        return {text, "-"};

    if (std::isupper(static_cast<unsigned char>(text[0]))) {
        const int ams_index = text[0] - 'A';
        if (text.size() == 1)
            return {std::to_string(ams_index), "-"};

        bool all_digits = true;
        for (size_t i = 1; i < text.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
                all_digits = false;
                break;
            }
        }

        if (all_digits) {
            int slot = 0;
            try {
                slot = std::stoi(text.substr(1)) - 1;
            } catch (...) {
                slot = -1;
            }
            return {std::to_string(ams_index), slot >= 0 ? std::to_string(slot) : std::string("-")};
        }
    }

    return {text, "-"};
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

void draw_outer_frame(wxDC& dc, const wxWindow& item, const wxColour& border_colour, int border_width, int corner_radius, bool fill)
{
    const wxSize size = item.GetClientSize();
    if (size.x <= 0 || size.y <= 0)
        return;

    dc.SetPen(wxPen(border_colour, border_width));
    dc.SetBrush(fill ? wxBrush(item.GetBackgroundColour().IsOk() ? item.GetBackgroundColour() : *wxWHITE) : *wxTRANSPARENT_BRUSH);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, corner_radius);
}

void draw_extra_content(wxDC& dc, const wxWindow& item, const wxSize& bambu_size, const wxString& slot_label, const wxFont& font, wxColour text_colour)
{
    const ItemState* state = state_for(item);
    if (!state)
        return;

    const wxSize size = item.GetClientSize();
    const int top_h = top_extra(item);
    const int bottom_y = top_h + bambu_size.y;

    dc.SetFont(font.IsOk() ? font : base_font(item, 12, wxFONTWEIGHT_NORMAL));
    dc.SetTextForeground(text_colour);
    draw_centered_text(dc, wxString::FromUTF8(format_weight(state->required_g)), 0, item.FromDIP(1), size.x, top_h - item.FromDIP(2));

    const ParsedSlot slot = parse_slot_label(slot_label);

    wxFont pill_font = font.IsOk() ? font : base_font(item, 12, wxFONTWEIGHT_NORMAL);
    dc.SetFont(pill_font);
    const wxString pill_text = visible_text(slot.ams_id);
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
    dc.SetTextForeground(text_colour);
    draw_centered_text(dc, wxString("S:") + visible_text(slot.slot_id), 0, slot_y, size.x, item.FromDIP(20));
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
    draw_extra_content(dc, item, bambu_size, slot_label, font, text_colour);
    draw_outer_frame(dc, item, border_colour, border_width, corner_radius, false);
}

void set_print_dialog_required_weight(wxWindow& item, std::optional<float> required_g)
{
    ItemState& state = s_item_states[&item];
    state.required_g = required_g;
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
