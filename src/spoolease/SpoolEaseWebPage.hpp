#ifndef spoolEaseWebPage_hpp_
#define spoolEaseWebPage_hpp_

class wxWindow;

namespace Slic3r { namespace SpoolEase {

wxWindow* create_web_page(wxWindow* parent);
void request_web_page_reload();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseWebPage_hpp_
