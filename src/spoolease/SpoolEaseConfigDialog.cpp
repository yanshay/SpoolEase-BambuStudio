#include "SpoolEaseConfigDialog.hpp"

#include "SpoolEaseConfig.hpp"
#include "SpoolEaseBackup.hpp"
#include "SpoolEaseCustomFilaments.hpp"
#include "SpoolEaseLog.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/RadioBox.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <boost/nowide/fstream.hpp>

#include <wx/clipbrd.h>
#include <wx/checkbox.h>
#include <wx/dataobj.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace Slic3r { namespace SpoolEase {

namespace {

const wxColour DESIGN_GRAY900_COLOR("#262E30");
const wxColour DESIGN_GRAY800_COLOR("#323A3D");
const wxColour DESIGN_GRAY600_COLOR("#6B6B6B");
const wxColour DESIGN_GRAY400_COLOR("#A6A9AA");
const wxColour DESIGN_GRAY300_COLOR("#EEEEEE");
const wxColour DESIGN_GRAY200_COLOR("#F8F8F8");
const wxColour DESIGN_GREEN_COLOR("#00AE42");
const wxColour DESIGN_RED_COLOR("#D01B1B");

class SpoolEaseScrolledWindow : public wxScrolledWindow
{
public:
    SpoolEaseScrolledWindow(wxWindow* parent)
        : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL)
    {}

    bool ShouldScrollToChildOnFocus(wxWindow*) override { return false; }
};

std::string to_utf8(const wxString& text)
{
    const wxScopedCharBuffer buffer = text.ToUTF8();
    return buffer.data() ? std::string(buffer.data()) : std::string();
}

std::string trim_copy(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
    return value;
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string normalize_pem_text(std::string text)
{
    replace_all(text, "\r\n", "\n");
    replace_all(text, "\r", "\n");
    replace_all(text, "\\n", "\n");
    return text;
}

std::string field_value(TextInput* control)
{
    return trim_copy(to_utf8(control->GetTextCtrl()->GetValue()));
}

std::string pem_value(wxTextCtrl* control)
{
    return trim_copy(normalize_pem_text(to_utf8(control->GetValue())));
}

wxColour themed(const wxColour& colour)
{
    return StateColor::darkModeColorFor(colour);
}

wxStaticText* make_label(wxWindow* parent, const wxString& text, const wxColour& colour = DESIGN_GRAY900_COLOR, const wxFont& font = Label::Body_13)
{
    auto* item = new wxStaticText(parent, wxID_ANY, text);
    item->SetBackgroundColour(*wxWHITE);
    item->SetForegroundColour(colour);
    item->SetFont(font);
    item->Wrap(-1);
    return item;
}

wxBoxSizer* make_section_title(wxWindow* parent, const wxString& title)
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* title_label = make_label(parent, title, DESIGN_GRAY800_COLOR, Label::Head_13);
    auto* line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line->SetBackgroundColour(DESIGN_GRAY400_COLOR);

    sizer->Add(title_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, parent->FromDIP(3));
    sizer->AddSpacer(parent->FromDIP(9));
    sizer->Add(line, 1, wxALIGN_CENTER_VERTICAL);
    return sizer;
}

StateColor secondary_button_background()
{
    return StateColor(
        std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal));
}

StateColor secondary_button_border()
{
    return StateColor(
        std::pair<wxColour, int>(wxColour("#909090"), StateColor::Disabled),
        std::pair<wxColour, int>(DESIGN_GRAY900_COLOR, StateColor::Normal));
}

StateColor secondary_button_text()
{
    return StateColor(
        std::pair<wxColour, int>(wxColour("#909090"), StateColor::Disabled),
        std::pair<wxColour, int>(DESIGN_GRAY900_COLOR, StateColor::Normal));
}

StateColor primary_button_background()
{
    return StateColor(
        std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#1B8844"), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour("#30DD70"), StateColor::Hovered),
        std::pair<wxColour, int>(DESIGN_GREEN_COLOR, StateColor::Normal));
}

StateColor primary_button_text()
{
    return StateColor(
        std::pair<wxColour, int>(wxColour("#909090"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal));
}

void style_button(Button* button, const wxSize& min_size, bool primary)
{
    if (primary) {
        const StateColor bg = primary_button_background();
        button->SetBackgroundColor(bg);
        button->SetBorderColor(bg);
        button->SetTextColor(primary_button_text());
    } else {
        button->SetBackgroundColor(secondary_button_background());
        button->SetBorderColor(secondary_button_border());
        button->SetTextColor(secondary_button_text());
    }

    button->SetFont(Label::Body_13);
    button->SetMinSize(min_size);
    button->SetPaddingSize(wxSize(button->FromDIP(12), button->FromDIP(5)));
    button->SetCornerRadius(button->FromDIP(12));
}

void style_text_input(TextInput* input)
{
    input->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#FFFFFF"), StateColor::Normal)));
    input->SetBorderColor(StateColor(
        std::pair<wxColour, int>(wxColour("#DBDBDB"), StateColor::Disabled),
        std::pair<wxColour, int>(DESIGN_GREEN_COLOR, StateColor::Hovered),
        std::pair<wxColour, int>(wxColour("#DBDBDB"), StateColor::Normal)));
    input->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour("#909090"), StateColor::Disabled),
        std::pair<wxColour, int>(DESIGN_GRAY900_COLOR, StateColor::Normal)));
    input->SetCornerRadius(input->FromDIP(4));
}

class ConfigDialog : public Slic3r::GUI::DPIDialog
{
public:
    explicit ConfigDialog(wxWindow* parent)
        : Slic3r::GUI::DPIDialog(parent, wxID_ANY, _L("SpoolEase Settings"), wxDefaultPosition, wxDefaultSize, wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX)
    {
        SPOOLEASE_LOG(info) << "SpoolEase: settings dialog opened";

        SetBackgroundColour(*wxWHITE);
        build_ui();
        load_config();
        update_certificate_controls();
        update_validity();
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
    }

    void on_dpi_changed(const wxRect&) override
    {
        for (TextInput* input : m_text_inputs)
            input->Rescale();
        for (Slic3r::GUI::RadioBox* radio : m_radios)
            radio->Rescale();
        for (Button* button : m_buttons)
            button->Rescale();

        if (m_ca_cert)
            m_ca_cert->SetMinSize(wxSize(FromDIP(470), FromDIP(150)));

        Layout();
        Refresh();
    }

private:
    TextInput* create_text_input(wxWindow* parent)
    {
        auto* input = new TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(340), -1), wxTE_PROCESS_ENTER);
        style_text_input(input);
        m_text_inputs.push_back(input);
        return input;
    }

    Button* create_button(wxWindow* parent, const wxString& label, const wxSize& min_size, bool primary, wxWindowID id = wxID_ANY)
    {
        auto* button = new Button(parent, label, wxEmptyString, 0, 0, id);
        style_button(button, min_size, primary);
        m_buttons.push_back(button);
        return button;
    }

    void build_ui()
    {
        auto* main_sizer = new wxBoxSizer(wxVERTICAL);

        m_scrolled_window = new SpoolEaseScrolledWindow(this);
        m_scrolled_window->SetBackgroundColour(*wxWHITE);
        m_scrolled_window->SetScrollRate(5, 5);
        m_scrolled_window->SetMinSize(wxSize(FromDIP(620), FromDIP(610)));

        auto* body_sizer = new wxBoxSizer(wxVERTICAL);
        auto* top_line = new wxPanel(m_scrolled_window, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(540), 1), wxTAB_TRAVERSAL);
        top_line->SetBackgroundColour(DESIGN_GRAY400_COLOR);
        body_sizer->Add(top_line, 0, wxEXPAND);
        body_sizer->AddSpacer(FromDIP(28));

        auto* page = new wxWindow(m_scrolled_window, wxID_ANY);
        page->SetBackgroundColour(*wxWHITE);
        auto* page_sizer = new wxBoxSizer(wxVERTICAL);

        page_sizer->Add(make_section_title(page, _L("Bambu Studio")), 0, wxEXPAND);
        add_auto_sync_row(page, page_sizer);
        add_backup_folder_row(page, page_sizer);

        page_sizer->Add(make_section_title(page, _L("Console")), 0, wxEXPAND | wxTOP, FromDIP(22));
        add_config_path(page, page_sizer);
        add_apply_note(page, page_sizer);

        m_address = create_text_input(page);
        m_security_key = create_text_input(page);
        m_api_token = create_text_input(page);
        add_text_row(page, page_sizer, _L("Console address"), m_address);
        add_text_row(page, page_sizer, _L("Security key"), m_security_key);
        add_text_row(page, page_sizer, _L("API token"), m_api_token);

        page_sizer->Add(make_section_title(page, _L("TLS / CA Certificate")), 0, wxEXPAND | wxTOP, FromDIP(22));
        m_no_verify = add_radio_row(page, page_sizer, _L("Do not verify the console TLS certificate"), false, FromDIP(8));
        m_provide_ca = add_radio_row(page, page_sizer, _L("Provide CA certificate"), true, FromDIP(4));

        auto* pem_button_row = new wxBoxSizer(wxHORIZONTAL);
        pem_button_row->AddSpacer(FromDIP(23));
        m_paste_button = create_button(page, _L("Paste from Clipboard"), wxSize(FromDIP(150), FromDIP(26)), false);
        m_load_button = create_button(page, _L("Load PEM File..."), wxSize(FromDIP(128), FromDIP(26)), false);
        pem_button_row->Add(m_paste_button, 0, wxRIGHT, FromDIP(8));
        pem_button_row->Add(m_load_button, 0);
        page_sizer->Add(pem_button_row, 0, wxTOP, FromDIP(8));

        m_ca_cert = new wxTextCtrl(page, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(470), FromDIP(150)), wxTE_MULTILINE | wxTE_DONTWRAP | wxHSCROLL | wxVSCROLL);
        wxFont pem_font = Label::Body_12;
        pem_font.SetFamily(wxFONTFAMILY_TELETYPE);
        m_ca_cert->SetFont(pem_font);
        m_ca_cert->SetBackgroundColour(*wxWHITE);
        m_ca_cert->SetForegroundColour(DESIGN_GRAY900_COLOR);
        m_ca_cert->SetHint(_L("Paste CA certificate PEM here"));

        auto* pem_row = new wxBoxSizer(wxHORIZONTAL);
        pem_row->AddSpacer(FromDIP(23));
        pem_row->Add(m_ca_cert, 1, wxEXPAND);
        page_sizer->Add(pem_row, 0, wxEXPAND | wxTOP, FromDIP(8));

        auto* api_note = make_label(page, _L("The API token and CA certificate can be obtained from the SpoolEase web application under Settings (Gear Icon) → API tab."), DESIGN_GRAY600_COLOR, Label::Body_13);
        api_note->Wrap(FromDIP(540));
        auto* api_note_row = new wxBoxSizer(wxHORIZONTAL);
        api_note_row->AddSpacer(FromDIP(23));
        api_note_row->Add(api_note, 1, wxEXPAND);
        page_sizer->Add(api_note_row, 0, wxEXPAND | wxTOP, FromDIP(8));

        auto* security_key_note = make_label(page, _L("SecurityKey is the key you set in the SpoolEase web config for signing in to the SpoolEase web application."), DESIGN_GRAY600_COLOR, Label::Body_13);
        security_key_note->Wrap(FromDIP(540));
        auto* security_key_note_row = new wxBoxSizer(wxHORIZONTAL);
        security_key_note_row->AddSpacer(FromDIP(23));
        security_key_note_row->Add(security_key_note, 1, wxEXPAND);
        page_sizer->Add(security_key_note_row, 0, wxEXPAND | wxTOP, FromDIP(4));

        m_status = make_label(page, wxEmptyString, DESIGN_GRAY600_COLOR, Label::Body_13);
        m_status->SetMinSize(wxSize(-1, FromDIP(20)));
        auto* status_row = new wxBoxSizer(wxHORIZONTAL);
        status_row->AddSpacer(FromDIP(23));
        status_row->Add(m_status, 1, wxEXPAND);
        page_sizer->Add(status_row, 0, wxEXPAND | wxTOP, FromDIP(8));

        page->SetSizer(page_sizer);
        body_sizer->Add(page, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
        body_sizer->AddSpacer(FromDIP(28));
        m_scrolled_window->SetSizerAndFit(body_sizer);
        main_sizer->Add(m_scrolled_window, 1, wxEXPAND);

        auto* bottom_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
        bottom_line->SetBackgroundColour(DESIGN_GRAY300_COLOR);
        main_sizer->Add(bottom_line, 0, wxEXPAND);

        auto* button_panel = new wxWindow(this, wxID_ANY);
        button_panel->SetBackgroundColour(*wxWHITE);
        auto* button_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_erase_button = create_button(button_panel, _L("Disable SpoolEase and Erase Config"), wxSize(FromDIP(240), FromDIP(28)), false);
        m_cancel_button = create_button(button_panel, _L("Cancel"), wxSize(FromDIP(78), FromDIP(28)), false, wxID_CANCEL);
        m_ok_button = create_button(button_panel, _L("OK"), wxSize(FromDIP(70), FromDIP(28)), true, wxID_OK);
        button_sizer->Add(m_erase_button, 0, wxRIGHT, FromDIP(8));
        button_sizer->AddStretchSpacer();
        button_sizer->Add(m_cancel_button, 0, wxRIGHT, FromDIP(8));
        button_sizer->Add(m_ok_button, 0);
        button_panel->SetSizer(button_sizer);
        main_sizer->Add(button_panel, 0, wxEXPAND | wxALL, FromDIP(18));

        SetSizer(main_sizer);
        SetMinSize(wxSize(FromDIP(660), FromDIP(740)));
        SetSize(wxSize(FromDIP(660), FromDIP(760)));
        Layout();
        CenterOnParent();
        SetEscapeId(wxID_CANCEL);
        SetAffirmativeId(wxID_OK);

        bind_events();
    }

    void add_config_path(wxWindow* parent, wxBoxSizer* sizer)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(parent->FromDIP(23));

        auto* title = make_label(parent, _L("Config file"));
        title->SetMinSize(wxSize(parent->FromDIP(108), -1));
        row->Add(title, 0, wxALIGN_TOP | wxALL, parent->FromDIP(3));

        m_path_label = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(config_file_path()), wxDefaultPosition, wxSize(parent->FromDIP(350), parent->FromDIP(44)), wxTE_MULTILINE | wxTE_READONLY | wxTE_CHARWRAP | wxTE_NO_VSCROLL | wxBORDER_NONE);
        m_path_label->SetBackgroundColour(*wxWHITE);
        m_path_label->SetForegroundColour(DESIGN_GRAY600_COLOR);
        m_path_label->SetFont(Label::Body_13);
        m_path_label->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
            wxMenu menu;
            menu.Append(wxID_COPY, _L("Copy"));
            m_path_label->PopupMenu(&menu);
        });
        m_path_label->Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_path_label->Copy(); }, wxID_COPY);
        row->Add(m_path_label, 1, wxALIGN_TOP | wxALL, parent->FromDIP(3));
        sizer->Add(row, 0, wxEXPAND | wxTOP, parent->FromDIP(10));
    }

    void add_apply_note(wxWindow* parent, wxBoxSizer* sizer)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(parent->FromDIP(23));
        auto* note = make_label(parent, _L("These settings are applied automatically after saving."), DESIGN_GRAY600_COLOR, Label::Body_13);
        note->Wrap(parent->FromDIP(470));
        row->Add(note, 1, wxEXPAND | wxALL, parent->FromDIP(3));
        sizer->Add(row, 0, wxEXPAND | wxTOP, parent->FromDIP(4));
    }

    void add_auto_sync_row(wxWindow* parent, wxBoxSizer* sizer)
    {
        auto* checkbox_row = new wxBoxSizer(wxHORIZONTAL);
        checkbox_row->AddSpacer(parent->FromDIP(23));
        m_auto_sync_custom_filaments = new wxCheckBox(parent, wxID_ANY, _L("Automatically sync custom filaments"));
        m_auto_sync_custom_filaments->SetBackgroundColour(*wxWHITE);
        m_auto_sync_custom_filaments->SetForegroundColour(DESIGN_GRAY900_COLOR);
        m_auto_sync_custom_filaments->SetFont(Label::Body_13);
        checkbox_row->Add(m_auto_sync_custom_filaments, 0, wxALIGN_CENTER_VERTICAL | wxALL, parent->FromDIP(3));
        sizer->Add(checkbox_row, 0, wxEXPAND | wxTOP, parent->FromDIP(10));

        auto* help_row = new wxBoxSizer(wxHORIZONTAL);
        help_row->AddSpacer(parent->FromDIP(46));
        auto* help = make_label(parent, _L("Syncs Bambu Studio custom filament settings to SpoolEase automatically. After changes, refreshes the SpoolEase page with updated filament information."), DESIGN_GRAY600_COLOR, Label::Body_13);
        help->Wrap(parent->FromDIP(470));
        help_row->Add(help, 1, wxEXPAND | wxALL, parent->FromDIP(3));
        sizer->Add(help_row, 0, wxEXPAND | wxTOP, parent->FromDIP(2));
    }

    void add_backup_folder_row(wxWindow* parent, wxBoxSizer* sizer)
    {
        m_backup_folder = create_text_input(parent);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(parent->FromDIP(23));

        auto* title_label = make_label(parent, _L("Backup folder"));
        title_label->SetMinSize(wxSize(parent->FromDIP(108), -1));
        row->Add(title_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, parent->FromDIP(3));
        row->Add(m_backup_folder, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));

        m_backup_folder_browse = create_button(parent, _L("Browse..."), wxSize(parent->FromDIP(92), parent->FromDIP(26)), false);
        row->Add(m_backup_folder_browse, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));
        sizer->Add(row, 0, wxEXPAND | wxTOP, parent->FromDIP(12));

        auto* help_row = new wxBoxSizer(wxHORIZONTAL);
        help_row->AddSpacer(parent->FromDIP(46));
        auto* help = make_label(parent, _L("Optional default folder for local SpoolEase backups. If empty, each backup will ask where to save."), DESIGN_GRAY600_COLOR, Label::Body_13);
        help->Wrap(parent->FromDIP(470));
        help_row->Add(help, 1, wxEXPAND | wxALL, parent->FromDIP(3));
        sizer->Add(help_row, 0, wxEXPAND | wxTOP, parent->FromDIP(2));
    }

    void add_text_row(wxWindow* parent, wxBoxSizer* sizer, const wxString& title, TextInput* input)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(parent->FromDIP(23));

        auto* title_label = make_label(parent, title);
        title_label->SetMinSize(wxSize(parent->FromDIP(108), -1));
        row->Add(title_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, parent->FromDIP(3));
        row->Add(input, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(8));
        sizer->Add(row, 0, wxEXPAND | wxTOP, parent->FromDIP(6));
    }

    Slic3r::GUI::RadioBox* add_radio_row(wxWindow* parent, wxBoxSizer* sizer, const wxString& title, bool provide_ca, int top_margin)
    {
        auto* row = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(28)));
        row->SetBackgroundColour(*wxWHITE);
        auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
        row_sizer->AddSpacer(parent->FromDIP(23));

        auto* radio = new Slic3r::GUI::RadioBox(row);
        auto* label = make_label(row, title);
        row_sizer->Add(radio, 0, wxALIGN_CENTER_VERTICAL);
        row_sizer->AddSpacer(parent->FromDIP(8));
        row_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxALL, parent->FromDIP(3));
        row->SetSizer(row_sizer);

        auto select = [this, provide_ca](wxEvent& event) {
            select_certificate_mode(provide_ca);
            event.Skip();
        };
        row->Bind(wxEVT_LEFT_DOWN, select);
        label->Bind(wxEVT_LEFT_DOWN, select);
        radio->Bind(wxEVT_TOGGLEBUTTON, [this, provide_ca](wxCommandEvent& event) {
            select_certificate_mode(provide_ca);
            event.Skip();
        });

        m_radios.push_back(radio);
        sizer->Add(row, 0, wxEXPAND | wxTOP, top_margin);
        return radio;
    }

    void bind_events()
    {
        auto mark_edited = [this](wxCommandEvent& event) {
            m_erase_requested = false;
            m_load_error.clear();
            update_validity();
            event.Skip();
        };

        m_address->GetTextCtrl()->Bind(wxEVT_TEXT, mark_edited);
        m_security_key->GetTextCtrl()->Bind(wxEVT_TEXT, mark_edited);
        m_api_token->GetTextCtrl()->Bind(wxEVT_TEXT, mark_edited);
        m_backup_folder->GetTextCtrl()->Bind(wxEVT_TEXT, mark_edited);
        m_ca_cert->Bind(wxEVT_TEXT, mark_edited);
        m_auto_sync_custom_filaments->Bind(wxEVT_CHECKBOX, mark_edited);

        m_paste_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { paste_certificate(); });
        m_load_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { load_certificate_file(); });
        m_backup_folder_browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose_backup_folder(); });
        m_erase_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { mark_for_erase(); });
        m_ok_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply(); });
        m_cancel_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    }

    void load_config()
    {
        std::string error;
        const std::optional<ConsoleConfig> config = console_config_for_edit(&error);
        if (config.has_value()) {
            m_address->GetTextCtrl()->ChangeValue(wxString::FromUTF8(config->address));
            m_security_key->GetTextCtrl()->ChangeValue(wxString::FromUTF8(config->security_key));
            m_api_token->GetTextCtrl()->ChangeValue(wxString::FromUTF8(config->api_token));
            m_backup_folder->GetTextCtrl()->ChangeValue(wxString::FromUTF8(config->backup_folder));
            m_ca_cert->ChangeValue(wxString::FromUTF8(normalize_pem_text(config->ca_cert_pem)));
            m_auto_sync_custom_filaments->SetValue(config->auto_sync_custom_filaments);
            m_provide_ca->SetValue(!config->ca_cert_pem.empty());
            m_no_verify->SetValue(config->ca_cert_pem.empty());
        } else {
            m_no_verify->SetValue(true);
            m_provide_ca->SetValue(false);
        }

        m_load_error = error;
    }

    void select_certificate_mode(bool provide_ca)
    {
        m_erase_requested = false;
        m_load_error.clear();
        m_provide_ca->SetValue(provide_ca);
        m_no_verify->SetValue(!provide_ca);
        update_certificate_controls();
        update_validity();
    }

    void update_certificate_controls()
    {
        const bool enabled = m_provide_ca->GetValue();
        m_ca_cert->Enable(enabled);
        m_ca_cert->SetBackgroundColour(themed(enabled ? wxColour("#FFFFFF") : wxColour("#F0F0F1")));
        m_ca_cert->SetForegroundColour(themed(enabled ? DESIGN_GRAY900_COLOR : wxColour("#909090")));
        m_paste_button->Enable(enabled);
        m_load_button->Enable(enabled);
        m_ca_cert->Refresh();
    }

    bool all_fields_empty() const
    {
        return field_value(m_address).empty() && field_value(m_security_key).empty() && field_value(m_api_token).empty() && field_value(m_backup_folder).empty() && (!m_provide_ca->GetValue() || pem_value(m_ca_cert).empty());
    }

    wxString validation_error() const
    {
        if (m_erase_requested || all_fields_empty())
            return wxEmptyString;

        if (field_value(m_address).empty())
            return _L("Console address is required.");
        if (field_value(m_security_key).empty())
            return _L("Security key is required.");
        if (field_value(m_api_token).empty())
            return _L("API token is required.");
        if (m_provide_ca->GetValue() && pem_value(m_ca_cert).empty())
            return _L("CA certificate is required when certificate verification is enabled.");

        return wxEmptyString;
    }

    void update_validity()
    {
        const wxString error = validation_error();
        m_ok_button->Enable(error.empty());

        if (!error.empty()) {
            set_status(error, true);
        } else if (!m_load_error.empty()) {
            set_status(wxString::Format(_L("Failed to read existing config: %s"), wxString::FromUTF8(m_load_error)), true);
        } else if (m_erase_requested) {
            set_status(_L("Click OK to erase config.json and disable SpoolEase integration."), false);
        } else if (all_fields_empty()) {
            set_status(_L("No SpoolEase config will be saved."), false);
        } else {
            set_status(_L("Click OK to save. Changes are applied automatically."), false);
        }
    }

    void set_status(const wxString& message, bool error)
    {
        m_status->SetForegroundColour(themed(error ? DESIGN_RED_COLOR : DESIGN_GRAY600_COLOR));
        m_status->SetLabel(message);
        m_status->Wrap(FromDIP(540));
        Layout();
    }

    void paste_certificate()
    {
        if (!wxTheClipboard || !wxTheClipboard->Open()) {
            set_status(_L("Failed to open clipboard."), true);
            return;
        }

        wxTextDataObject data;
        const bool got_text = wxTheClipboard->GetData(data);
        wxTheClipboard->Close();

        if (!got_text) {
            set_status(_L("Clipboard does not contain text."), true);
            return;
        }

        m_erase_requested = false;
        m_load_error.clear();
        m_ca_cert->SetValue(wxString::FromUTF8(normalize_pem_text(to_utf8(data.GetText()))));
        update_validity();
    }

    void load_certificate_file()
    {
        wxFileDialog dialog(this, _L("Load CA certificate"), wxEmptyString, wxEmptyString, _L("PEM files (*.pem;*.crt;*.cer)|*.pem;*.crt;*.cer|All files (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;

        const std::string path = to_utf8(dialog.GetPath());
        boost::nowide::ifstream ifs(path, std::ios::in | std::ios::binary);
        if (!ifs) {
            set_status(_L("Failed to read CA certificate file."), true);
            return;
        }

        std::ostringstream buffer;
        buffer << ifs.rdbuf();
        m_erase_requested = false;
        m_load_error.clear();
        m_ca_cert->SetValue(wxString::FromUTF8(normalize_pem_text(buffer.str())));
        update_validity();
    }

    void choose_backup_folder()
    {
        wxDirDialog dialog(this, _L("Choose SpoolEase backup folder"), wxString::FromUTF8(field_value(m_backup_folder)), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);
        if (dialog.ShowModal() != wxID_OK)
            return;

        m_erase_requested = false;
        m_load_error.clear();
        m_backup_folder->GetTextCtrl()->SetValue(dialog.GetPath());
        update_validity();
    }

    void mark_for_erase()
    {
        m_erase_requested = true;
        m_load_error.clear();
        m_address->GetTextCtrl()->ChangeValue(wxEmptyString);
        m_security_key->GetTextCtrl()->ChangeValue(wxEmptyString);
        m_api_token->GetTextCtrl()->ChangeValue(wxEmptyString);
        m_backup_folder->GetTextCtrl()->ChangeValue(wxEmptyString);
        m_ca_cert->ChangeValue(wxEmptyString);
        m_auto_sync_custom_filaments->SetValue(false);
        m_no_verify->SetValue(true);
        m_provide_ca->SetValue(false);
        update_certificate_controls();
        update_validity();
    }

    void apply()
    {
        const wxString error = validation_error();
        if (!error.empty()) {
            update_validity();
            return;
        }

        std::string io_error;
        if (m_erase_requested || all_fields_empty()) {
            SPOOLEASE_LOG(info) << "SpoolEase: settings apply: action=" << (m_erase_requested ? "erase" : "no_config");
            if (!delete_console_config(&io_error)) {
                set_status(wxString::Format(_L("Failed to erase config: %s"), wxString::FromUTF8(io_error)), true);
                return;
            }
            EndModal(wxID_OK);
            return;
        }

        ConsoleConfig config;
        config.address = field_value(m_address);
        config.security_key = field_value(m_security_key);
        config.api_token = field_value(m_api_token);
        config.backup_folder = field_value(m_backup_folder);
        config.ca_cert_pem = m_provide_ca->GetValue() ? pem_value(m_ca_cert) : std::string();
        config.auto_sync_custom_filaments = m_auto_sync_custom_filaments->GetValue();

        SPOOLEASE_LOG(info) << "SpoolEase: settings apply: action=save";
        if (!save_console_config(config, &io_error)) {
            set_status(wxString::Format(_L("Failed to save config: %s"), wxString::FromUTF8(io_error)), true);
            return;
        }

        EndModal(wxID_OK);
    }

private:
    SpoolEaseScrolledWindow*       m_scrolled_window{nullptr};
    wxTextCtrl*                    m_path_label{nullptr};
    TextInput*                     m_address{nullptr};
    TextInput*                     m_security_key{nullptr};
    TextInput*                     m_api_token{nullptr};
    TextInput*                     m_backup_folder{nullptr};
    wxCheckBox*                    m_auto_sync_custom_filaments{nullptr};
    Slic3r::GUI::RadioBox*         m_no_verify{nullptr};
    Slic3r::GUI::RadioBox*         m_provide_ca{nullptr};
    wxTextCtrl*                    m_ca_cert{nullptr};
    Button*                        m_paste_button{nullptr};
    Button*                        m_load_button{nullptr};
    Button*                        m_backup_folder_browse{nullptr};
    wxStaticText*                  m_status{nullptr};
    Button*                        m_erase_button{nullptr};
    Button*                        m_cancel_button{nullptr};
    Button*                        m_ok_button{nullptr};
    std::vector<TextInput*>        m_text_inputs;
    std::vector<Slic3r::GUI::RadioBox*> m_radios;
    std::vector<Button*>           m_buttons;
    std::string                    m_load_error;
    bool                           m_erase_requested{false};
};

void show_config_dialog(wxWindow& parent)
{
    ConfigDialog dialog(&parent);
    dialog.ShowModal();
}

wxMenu* create_config_menu(wxWindow& parent)
{
    auto* menu = new wxMenu();
    wxMenuItem* item = menu->Append(wxID_ANY, _L("Settings"), _L("Edit SpoolEase integration settings"));
    menu->Bind(wxEVT_MENU, [&parent](wxCommandEvent&) { show_config_dialog(parent); }, item->GetId());
    wxMenuItem* backup_item = menu->Append(wxID_ANY, _L("Backup..."), _L("Save a SpoolEase backup to this computer"));
    menu->Bind(wxEVT_MENU, [&parent](wxCommandEvent&) { backup_to_local_disk(parent); }, backup_item->GetId());
    wxMenuItem* sync_item = menu->Append(wxID_ANY, _L("Sync Custom Filaments"), _L("Submit custom filament settings to SpoolEase"));
    menu->Bind(wxEVT_MENU, [&parent](wxCommandEvent&) { sync_custom_filaments(parent); }, sync_item->GetId());
    return menu;
}

} // namespace

void install_config_menu(wxWindow& parent, wxMenuBar* menubar, AddTopbarSubmenuFn add_topbar_submenu)
{
    start_custom_filament_auto_sync();

    wxMenu* menu = create_config_menu(parent);
    const wxString title = _L("SpoolEase");

#ifdef __APPLE__
    if (menubar) {
        const int help_index = menubar->FindMenu(_L("Help"));
        if (help_index != wxNOT_FOUND)
            menubar->Insert(help_index, menu, wxString::Format("&%s", title));
        else
            menubar->Append(menu, wxString::Format("&%s", title));
        SPOOLEASE_LOG(info) << "SpoolEase: settings menu installed";
        return;
    }
#endif

    if (add_topbar_submenu) {
        add_topbar_submenu(menu, title);
        SPOOLEASE_LOG(info) << "SpoolEase: settings menu installed";
        return;
    }

    if (menubar) {
        const int help_index = menubar->FindMenu(_L("Help"));
        if (help_index != wxNOT_FOUND)
            menubar->Insert(help_index, menu, title);
        else
            menubar->Append(menu, title);
        SPOOLEASE_LOG(info) << "SpoolEase: settings menu installed";
        return;
    }

    delete menu;
}

}} // namespace Slic3r::SpoolEase
