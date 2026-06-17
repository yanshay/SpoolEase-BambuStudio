#ifndef spoolEaseInventory_hpp_
#define spoolEaseInventory_hpp_

#include <optional>
#include <string>

namespace Slic3r { namespace SpoolEase {

struct SlotInventory
{
    std::string          spool_id;
    std::optional<float> weight_net;
    std::string          brand;
    std::string          material_type;
    std::string          material_subtype;
    std::string          color_name;
};

void start_inventory_polling();
void refresh_inventory_now();
std::optional<SlotInventory> slot_inventory(const std::string& printer_serial, const std::string& ams_id, const std::string& slot_id);
std::string display_spool_id(const std::string& spool_id);
std::string display_spool_details(const SlotInventory& inventory);
std::string display_weight(std::optional<float> weight);

}} // namespace Slic3r::SpoolEase

#endif // spoolEaseInventory_hpp_
