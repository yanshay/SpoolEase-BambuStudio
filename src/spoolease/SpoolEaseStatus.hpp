#ifndef spoolEaseStatus_hpp_
#define spoolEaseStatus_hpp_

#include <string>

class wxWindow;

namespace Slic3r { namespace SpoolEase {

void register_status_page(wxWindow* page);
void unregister_status_page(wxWindow* page);

void set_live_status_warning(const std::string& key, const std::string& message);
void set_live_status_error(const std::string& key, const std::string& message);
void clear_live_status(const std::string& key);

void set_sticky_status_error(const std::string& key, const std::string& message);
void clear_sticky_status(const std::string& key);

bool has_dismissible_error_status();
void dismiss_error_status();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseStatus_hpp_
