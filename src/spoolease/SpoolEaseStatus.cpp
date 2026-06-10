#include "SpoolEaseStatus.hpp"

#include "slic3r/GUI/Notebook.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"

#include <wx/app.h>
#include <wx/msgdlg.h>
#include <wx/window.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

namespace Slic3r { namespace SpoolEase {

namespace {

enum class Severity
{
    Warning,
    Error
};

struct Issue
{
    Severity    severity{Severity::Warning};
    bool        sticky{false};
    std::string message;
};

std::mutex                   s_mutex;
std::map<std::string, Issue> s_issues;
wxWindow*                    s_status_page{nullptr};
Button*                      s_tab_button{nullptr};
bool                         s_lookup_warning_shown{false};

wxString trimmed_label(wxWindow* window)
{
    wxString label = window ? window->GetLabel() : wxString();
    label.Trim(true);
    label.Trim(false);
    return label;
}

Button* find_spoolease_tab_button(wxWindow* page)
{
    if (!page)
        return nullptr;

    auto* notebook = dynamic_cast<Notebook*>(page->GetParent());
    if (!notebook)
        return nullptr;

    ButtonsListCtrl* buttons = notebook->GetBtnsListCtrl();
    if (!buttons)
        return nullptr;

    const wxWindowList& children = buttons->GetChildren();
    for (wxWindowList::compatibility_iterator node = children.GetFirst(); node; node = node->GetNext()) {
        wxWindow* child = node->GetData();
        auto* button = dynamic_cast<Button*>(child);
        if (button && trimmed_label(button) == wxString::FromUTF8("SpoolEase"))
            return button;
    }

    return nullptr;
}

std::vector<Issue> current_issues()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    std::vector<Issue> issues;
    issues.reserve(s_issues.size());
    for (const auto& item : s_issues)
        issues.push_back(item.second);
    return issues;
}

wxString tooltip_for_issues(const std::vector<Issue>& issues)
{
    if (issues.empty())
        return wxString::FromUTF8("SpoolEase");

    wxString tooltip;

    auto append_group = [&tooltip, &issues](Severity severity, const wxString& title) {
        bool any = false;
        for (const Issue& issue : issues) {
            if (issue.severity != severity)
                continue;
            if (!any) {
                if (!tooltip.empty())
                    tooltip += "\n";
                tooltip += title;
                any = true;
            }
            tooltip += "\n- ";
            tooltip += wxString::FromUTF8(issue.message);
        }
    };

    append_group(Severity::Error, wxString::FromUTF8("Errors:"));
    append_group(Severity::Warning, wxString::FromUTF8("Warnings:"));
    return tooltip;
}

const char* icon_for_issues(const std::vector<Issue>& issues)
{
    const bool has_error = std::any_of(issues.begin(), issues.end(), [](const Issue& issue) { return issue.severity == Severity::Error; });
    if (has_error)
        return "error";

    const bool has_warning = std::any_of(issues.begin(), issues.end(), [](const Issue& issue) { return issue.severity == Severity::Warning; });
    if (has_warning)
        return "warning";

    return "tab_filament_active";
}

void apply_status_to_tab()
{
    wxWindow* page = nullptr;
    Button* button = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        page = s_status_page;
        button = s_tab_button;
    }

    if (!button) {
        button = find_spoolease_tab_button(page);
        if (button) {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (page == s_status_page)
                s_tab_button = button;
        }
    }

    if (!button) {
        bool show_warning = false;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            show_warning = !s_lookup_warning_shown && s_status_page;
            s_lookup_warning_shown = true;
        }
        if (show_warning)
            wxMessageBox(wxString::FromUTF8("SpoolEase could not locate its navigation tab button for status display."), wxString::FromUTF8("SpoolEase Status"), wxOK | wxICON_WARNING);
        return;
    }

    const std::vector<Issue> issues = current_issues();
    const wxString icon = wxString::FromUTF8(icon_for_issues(issues));
    button->SetIcon(icon);
    button->SetInactiveIcon(icon);
    button->SetToolTip(tooltip_for_issues(issues));
    button->Refresh();
}

void schedule_apply_status()
{
    if (!wxTheApp)
        return;

    wxTheApp->CallAfter([]() { apply_status_to_tab(); });
}

void set_status(const std::string& key, Severity severity, bool sticky, const std::string& message)
{
    if (key.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_issues[key] = Issue{severity, sticky, message};
    }
    schedule_apply_status();
}

void clear_status(const std::string& key, bool sticky)
{
    if (key.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const auto it = s_issues.find(key);
        if (it == s_issues.end() || it->second.sticky != sticky)
            return;
        s_issues.erase(it);
    }
    schedule_apply_status();
}

} // namespace

void register_status_page(wxWindow* page)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status_page = page;
        s_tab_button = nullptr;
        s_lookup_warning_shown = false;
    }
    schedule_apply_status();
}

void unregister_status_page(wxWindow* page)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_status_page == page) {
        s_status_page = nullptr;
        s_tab_button = nullptr;
    }
}

void set_live_status_warning(const std::string& key, const std::string& message)
{
    set_status(key, Severity::Warning, false, message);
}

void set_live_status_error(const std::string& key, const std::string& message)
{
    set_status(key, Severity::Error, false, message);
}

void clear_live_status(const std::string& key)
{
    clear_status(key, false);
}

void set_sticky_status_error(const std::string& key, const std::string& message)
{
    set_status(key, Severity::Error, true, message);
}

void clear_sticky_status(const std::string& key)
{
    clear_status(key, true);
}

bool has_dismissible_error_status()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return std::any_of(s_issues.begin(), s_issues.end(), [](const auto& item) { return item.second.sticky && item.second.severity == Severity::Error; });
}

void dismiss_error_status()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto it = s_issues.begin(); it != s_issues.end();) {
            if (it->second.sticky && it->second.severity == Severity::Error)
                it = s_issues.erase(it);
            else
                ++it;
        }
    }
    schedule_apply_status();
}

}} // namespace Slic3r::SpoolEase
