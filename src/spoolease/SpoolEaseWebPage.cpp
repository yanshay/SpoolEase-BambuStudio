#include "SpoolEaseWebPage.hpp"

#include "SpoolEaseConfig.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"

#include <wx/panel.h>
#include <wx/sizer.h>

namespace Slic3r { namespace SpoolEase {

class SpoolEaseWebPage : public wxPanel
{
public:
    explicit SpoolEaseWebPage(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    {
        m_webview = new Slic3r::GUI::PrinterWebView(this);
        m_webview->SetMinSize(wxSize(FromDIP(320), FromDIP(260)));

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_webview, 1, wxEXPAND);
        SetSizer(sizer);

        const std::string url = filament_manager_url();
        if (!url.empty())
            m_webview->load_url(wxString::FromUTF8(url));

        Layout();
        sizer->Layout();
        m_webview->Layout();
        Fit();
    }

private:
    Slic3r::GUI::PrinterWebView* m_webview{nullptr}; // owned by wx parent
};

wxWindow* create_web_page(wxWindow* parent)
{
    return new SpoolEaseWebPage(parent);
}

}} // namespace Slic3r::SpoolEase
