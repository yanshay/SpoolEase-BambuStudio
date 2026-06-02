#include "SpoolEaseWebPage.hpp"

#include "SpoolEaseConfig.hpp"

#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/app.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/webview.h>

namespace Slic3r { namespace SpoolEase {

namespace {

constexpr int retry_interval_ms = 5000;

class SpoolEaseWebPage : public wxPanel
{
public:
    explicit SpoolEaseWebPage(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
        , m_retry_timer(this)
    {
        m_webview = new Slic3r::GUI::PrinterWebView(this);
        m_webview->SetMinSize(wxSize(FromDIP(320), FromDIP(260)));

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_webview, 1, wxEXPAND);
        SetSizer(sizer);

        bind_events();
        load_configured_url(true);

        Layout();
        sizer->Layout();
        resize_webview();
        Fit();
    }

    ~SpoolEaseWebPage() override
    {
        if (wxTheApp)
            wxTheApp->Unbind(EVT_SPOOLEASE_CONFIG_CHANGED, &SpoolEaseWebPage::on_config_changed, this);
    }

private:
    void bind_events()
    {
        Bind(wxEVT_SIZE, &SpoolEaseWebPage::on_size, this);
        Bind(wxEVT_SHOW, &SpoolEaseWebPage::on_show, this);
        Bind(wxEVT_TIMER, &SpoolEaseWebPage::on_retry_timer, this);

        if (wxTheApp)
            wxTheApp->Bind(EVT_SPOOLEASE_CONFIG_CHANGED, &SpoolEaseWebPage::on_config_changed, this);

        if (wxWebView* webview = m_webview ? m_webview->GetWebView() : nullptr) {
            webview->Bind(wxEVT_WEBVIEW_ERROR, &SpoolEaseWebPage::on_webview_error, this);
            webview->Bind(wxEVT_WEBVIEW_LOADED, &SpoolEaseWebPage::on_webview_loaded, this);
        }
    }

    void on_size(wxSizeEvent& event)
    {
        resize_webview();
        event.Skip();
    }

    void on_show(wxShowEvent& event)
    {
        if (event.IsShown()) {
            CallAfter([this]() {
                resize_webview();
                if (!m_current_url.empty() && !m_page_loaded)
                    load_current_url();
            });
        }
        event.Skip();
    }

    void on_config_changed(wxCommandEvent&)
    {
        load_configured_url(true);
    }

    void on_webview_error(wxWebViewEvent& event)
    {
        if (event.GetURL() != "about:blank") {
            m_page_loaded = false;
            start_retry_timer();
        }
        event.Skip();
    }

    void on_webview_loaded(wxWebViewEvent& event)
    {
        if (!m_current_url.empty() && event.GetURL() != "about:blank") {
            m_page_loaded = true;
            m_retry_timer.Stop();
        }
        event.Skip();
    }

    void on_retry_timer(wxTimerEvent&)
    {
        if (!m_current_url.empty() && !m_page_loaded)
            load_current_url();
    }

    void resize_webview()
    {
        if (!m_webview)
            return;

        const wxSize size = GetClientSize();
        if (size.GetWidth() <= 0 || size.GetHeight() <= 0)
            return;

        m_webview->SetSize(size);
        if (wxWebView* webview = m_webview->GetWebView())
            webview->SetSize(m_webview->GetClientSize());
        m_webview->Layout();
    }

    void load_configured_url(bool force)
    {
        const std::string url = inventory_url();
        if (url.empty()) {
            clear_page();
            return;
        }

        if (!force && m_current_url == url && m_page_loaded)
            return;

        m_current_url = url;
        m_page_loaded = false;
        load_current_url();
    }

    void load_current_url()
    {
        if (!m_webview || m_current_url.empty())
            return;

        resize_webview();
        if (wxWebView* webview = m_webview->GetWebView()) {
            WebView::LoadUrl(webview, wxString::FromUTF8(m_current_url));
            start_retry_timer();
        }
    }

    void clear_page()
    {
        m_current_url.clear();
        m_page_loaded = false;
        m_retry_timer.Stop();

        if (m_webview) {
            if (wxWebView* webview = m_webview->GetWebView())
                WebView::LoadUrl(webview, "about:blank");
        }
    }

    void start_retry_timer()
    {
        if (!m_current_url.empty() && !m_page_loaded)
            m_retry_timer.Start(retry_interval_ms, wxTIMER_ONE_SHOT);
    }

private:
    Slic3r::GUI::PrinterWebView* m_webview{nullptr}; // owned by wx parent
    wxTimer                      m_retry_timer;
    std::string                  m_current_url;
    bool                         m_page_loaded{false};
};

} // namespace

wxWindow* create_web_page(wxWindow* parent)
{
    return new SpoolEaseWebPage(parent);
}

}} // namespace Slic3r::SpoolEase
