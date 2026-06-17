#include "SpoolEaseDropdownTooltip.hpp"

#include <wx/display.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/window.h>

#include <algorithm>

namespace Slic3r { namespace SpoolEase {

namespace {

struct DropdownTooltipState
{
    wxPopupWindow* popup{nullptr};
    wxStaticText*  label{nullptr};
    wxWindow*      owner{nullptr};
    wxString       text;
};

DropdownTooltipState& state()
{
    static DropdownTooltipState s;
    return s;
}

void destroy_popup()
{
    DropdownTooltipState& s = state();
    if (s.popup)
        s.popup->Destroy();
    s.popup = nullptr;
    s.label = nullptr;
    s.owner = nullptr;
    s.text.clear();
}

void ensure_popup(wxWindow& owner)
{
    DropdownTooltipState& s = state();
    if (s.popup && s.owner == &owner)
        return;

    destroy_popup();

    wxWindow* parent = wxGetTopLevelParent(&owner);
    if (!parent)
        parent = &owner;

    s.popup = new wxPopupWindow(parent, wxBORDER_SIMPLE);
    s.popup->SetBackgroundColour(wxColour(255, 255, 225));
    s.label = new wxStaticText(s.popup, wxID_ANY, wxEmptyString);
    s.label->SetForegroundColour(*wxBLACK);
    s.label->SetBackgroundColour(wxColour(255, 255, 225));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(s.label, 0, wxALL, owner.FromDIP(6));
    s.popup->SetSizer(sizer);
    s.owner = &owner;
}

wxPoint tooltip_position(wxWindow& owner, const wxSize& popup_size)
{
    wxPoint pos = wxGetMousePosition() + wxPoint(owner.FromDIP(16), owner.FromDIP(18));
    const int display_index = wxDisplay::GetFromPoint(pos);
    if (display_index != wxNOT_FOUND) {
        const wxRect area = wxDisplay(display_index).GetClientArea();
        if (pos.x + popup_size.x > area.GetRight())
            pos.x = area.GetRight() - popup_size.x;
        if (pos.y + popup_size.y > area.GetBottom())
            pos.y = area.GetBottom() - popup_size.y;
        pos.x = std::max(pos.x, area.GetLeft());
        pos.y = std::max(pos.y, area.GetTop());
    }
    return pos;
}

} // namespace

void show_dropdown_tooltip(wxWindow& owner, const wxString& text)
{
    if (text.empty() || !owner.IsShownOnScreen()) {
        hide_dropdown_tooltip(&owner);
        return;
    }

    ensure_popup(owner);

    DropdownTooltipState& s = state();
    if (!s.popup || !s.label)
        return;

    if (s.text != text) {
        s.text = text;
        s.label->SetLabel(text);
        s.popup->Fit();
    }

    s.popup->Move(tooltip_position(owner, s.popup->GetSize()));
    if (!s.popup->IsShown())
        s.popup->Show();
}

void hide_dropdown_tooltip(wxWindow* owner)
{
    DropdownTooltipState& s = state();
    if (owner && s.owner != owner)
        return;
    destroy_popup();
}

}} // namespace Slic3r::SpoolEase
