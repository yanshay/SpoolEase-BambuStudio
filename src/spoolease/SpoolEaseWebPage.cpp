#include "SpoolEaseWebPage.hpp"

#include "SpoolEaseConfig.hpp"

#include "libslic3r/Utils.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <wx/app.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include <iomanip>
#include <optional>
#include <sstream>
#include <vector>

namespace Slic3r { namespace SpoolEase {

namespace {

constexpr int retry_interval_ms = 5000;

namespace fs = boost::filesystem;

enum class StatusPage
{
    NotConfigured,
    NotAvailable
};

std::string fallback_status_template()
{
    return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{title}}</title>
<style>
body { display: flex; align-items: center; justify-content: center; min-height: 100vh; margin: 0; padding: 32px; background: {{page_bg}}; color: {{text}}; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Arial, sans-serif; }
.card { width: min(520px, 100%); padding: 34px 36px 32px; border: 1px solid {{border}}; border-radius: 14px; background: {{card_bg}}; text-align: center; box-shadow: 0 12px 32px rgba(0, 0, 0, 0.08); }
.status-icon { display: inline-flex; align-items: center; justify-content: center; width: 46px; height: 46px; margin-bottom: 18px; border: 2px solid {{accent}}; border-radius: 50%; color: {{accent}}; font-size: 26px; font-weight: 600; }
h1 { margin: 0; font-size: 22px; font-weight: 600; }
.message { margin: 14px 0 0; font-size: 14px; line-height: 1.55; }
.detail { margin: 8px 0 0; color: {{muted}}; font-size: 13px; line-height: 1.5; }
</style>
</head>
<body><main class="card" role="status" aria-live="polite"><div class="status-icon" aria-hidden="true">!</div><h1>{{title}}</h1><p class="message">{{message}}</p><p class="detail">{{detail}}</p></main></body>
</html>)HTML";
}

std::string read_text_file(const fs::path& path)
{
    boost::nowide::ifstream ifs(path.string());
    if (!ifs)
        return {};

    std::ostringstream buffer;
    buffer << ifs.rdbuf();
    return buffer.str();
}

std::string status_template()
{
    std::vector<fs::path> candidates;

#ifdef SPOOLEASE_SOURCE_DIR
    candidates.emplace_back(fs::path(SPOOLEASE_SOURCE_DIR) / "resources" / "web" / "unavailable.html");
#endif

    if (!Slic3r::resources_dir().empty())
        candidates.emplace_back(fs::path(Slic3r::resources_dir()) / "web" / "unavailable.html");

    for (const fs::path& candidate : candidates) {
        if (!fs::exists(candidate))
            continue;
        std::string html = read_text_file(candidate);
        if (!html.empty())
            return html;
    }

    return fallback_status_template();
}

std::string colour_to_html(const wxColour& colour)
{
    std::ostringstream ss;
    ss << '#'
       << std::uppercase << std::hex << std::setfill('0')
       << std::setw(2) << static_cast<int>(colour.Red())
       << std::setw(2) << static_cast<int>(colour.Green())
       << std::setw(2) << static_cast<int>(colour.Blue());
    return ss.str();
}

std::string bambu_colour(const wxColour& light_colour)
{
    if (Slic3r::GUI::wxGetApp().dark_mode())
        return colour_to_html(StateColor::darkModeColorFor(light_colour));
    return colour_to_html(light_colour);
}

void replace_all(std::string& text, const std::string& from, const std::string& to)
{
    if (from.empty())
        return;

    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.length(), to);
        pos += to.length();
    }
}

std::string escape_html(const std::string& text)
{
    std::string result;
    result.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += ch; break;
        }
    }
    return result;
}

std::string status_title(StatusPage status)
{
    return status == StatusPage::NotConfigured ? "SpoolEase is not configured" : "SpoolEase is not available";
}

std::string status_message(StatusPage status)
{
    if (status == StatusPage::NotConfigured)
        return "Open SpoolEase > Settings to configure the SpoolEase console connection.";
    return "Check that the SpoolEase console is running and reachable from this computer.";
}

std::string status_detail(StatusPage status)
{
    if (status == StatusPage::NotConfigured)
        return "SpoolEase inventory will appear here after settings are saved.";
    return "Bambu Studio will keep retrying automatically every 5 seconds.";
}

std::string render_status_page(StatusPage status)
{
    std::string html = status_template();

    replace_all(html, "{{title}}", escape_html(status_title(status)));
    replace_all(html, "{{message}}", escape_html(status_message(status)));
    replace_all(html, "{{detail}}", escape_html(status_detail(status)));
    replace_all(html, "{{page_bg}}", bambu_colour(wxColour("#F8F8F8")));
    replace_all(html, "{{card_bg}}", bambu_colour(wxColour("#FFFFFF")));
    replace_all(html, "{{text}}", bambu_colour(wxColour("#262E30")));
    replace_all(html, "{{muted}}", bambu_colour(wxColour("#6B6B6B")));
    replace_all(html, "{{border}}", bambu_colour(wxColour("#CECECE")));
    replace_all(html, "{{accent}}", bambu_colour(wxColour("#00AE42")));

    return html;
}

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
                if (m_status_page.has_value())
                    show_status_page(*m_status_page);
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
            show_status_page(StatusPage::NotAvailable);
            start_retry_timer();
        }
        event.Skip();
    }

    void on_webview_loaded(wxWebViewEvent& event)
    {
        if (!m_current_url.empty() && !event.GetURL().empty() && event.GetURL() != "about:blank") {
            m_page_loaded = true;
            m_status_page.reset();
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
            clear_page(StatusPage::NotConfigured);
            return;
        }

        if (!force && m_current_url == url && m_page_loaded)
            return;

        m_current_url = url;
        m_page_loaded = false;
        show_status_page(StatusPage::NotAvailable);
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

    void clear_page(StatusPage status)
    {
        m_current_url.clear();
        m_page_loaded = false;
        m_retry_timer.Stop();
        show_status_page(status);
    }

    void show_status_page(StatusPage status)
    {
        m_status_page = status;

        if (!m_webview)
            return;
        if (wxWebView* webview = m_webview->GetWebView())
            webview->SetPage(wxString::FromUTF8(render_status_page(status)), "about:blank");
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
    std::optional<StatusPage>    m_status_page;
    bool                         m_page_loaded{false};
};

} // namespace

wxWindow* create_web_page(wxWindow* parent)
{
    return new SpoolEaseWebPage(parent);
}

}} // namespace Slic3r::SpoolEase
