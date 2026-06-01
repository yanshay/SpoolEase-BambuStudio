#ifndef spoolEaseConfigDialog_hpp_
#define spoolEaseConfigDialog_hpp_

#include <functional>

#include <wx/string.h>

class wxMenu;
class wxMenuBar;
class wxWindow;

namespace Slic3r { namespace SpoolEase {

using AddTopbarSubmenuFn = std::function<void(wxMenu*, const wxString&)>;

void install_config_menu(wxWindow& parent, wxMenuBar* menubar, AddTopbarSubmenuFn add_topbar_submenu);

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseConfigDialog_hpp_
