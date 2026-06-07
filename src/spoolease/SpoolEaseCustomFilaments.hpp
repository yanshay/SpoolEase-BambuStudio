#ifndef spoolEaseCustomFilaments_hpp_
#define spoolEaseCustomFilaments_hpp_

class wxWindow;

namespace Slic3r { namespace SpoolEase {

void sync_custom_filaments(wxWindow& parent);
void start_custom_filament_auto_sync();

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseCustomFilaments_hpp_
