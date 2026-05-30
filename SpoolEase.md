# SpoolEase Integration Handoff

## Goal

Integrate external SpoolEase filament inventory data into Bambu Studio UI with the smallest possible edits to upstream Bambu source files. Rebase simplicity is the main design constraint.

SpoolEase is the source of truth for:

- Spool ID.
- Spool metadata.
- Available net weight on each spool.
- Which spool is currently loaded in each AMS slot and external slot.
- Real-time inventory/slot status.

Bambu Studio should only provide:

- UI context: selected printer, AMS id, slot id, external slot identity.
- Required filament amount per material in the print dialog.

Do not derive SpoolEase display data from Bambu's existing AMS remain/weight fields or any local inventory-like feature. If a future branch contains similar fields, they are not the source for this integration.

## Current Branch Review

This handoff was rechecked on branch `spoolease`. The relevant integration points still exist, with these branch-specific findings:

- `src/slic3r/GUI/fila_manager/` is present on this branch.
- `wxGetApp().fila_manager_store()`, `wxGetApp().fila_manager_sync()`, `wxGetApp().fila_manager_cloud_client()`, `wxGetApp().fila_manager_cloud_sync()`, and `wxGetApp().fila_manager_cloud_disp()` are present on this branch.
- The active user-visible “Filament Manager” tab is not the older `wgtFilaManagerPanel` WebView path. It is created as `DeviceWebPage` in `src/slic3r/GUI/MainFrame.cpp` and backed by `src/slic3r/GUI/DeviceWeb/FilaManagerVM.cpp` plus the React app under `src/slic3r/GUI/DeviceWeb/device_page/`.
- The older `wgtFilaManagerPanel` and `resources/web/fila_manager/` files still exist, but they do not appear to be the active main-tab UI path on this branch.
- `AmsItem::UpdateDeviceInfo(...)` is not present on this branch and is not needed for the SpoolEase hooks.
- `SelectMachineDialog::CheckWarningFilamentRemain(...)` still contains a useful required-grams calculation, but the call site is currently commented out. Required grams can also be taken from `FilamentInfo::used_g` where populated.
- `src/slic3r/CMakeLists.txt` currently has `add_subdirectory(GUI/DeviceCore)`, `add_subdirectory(GUI/DeviceTab)`, `add_subdirectory(GUI/fila_manager)`, and `add_subdirectory(GUI/DeviceWeb)`, but no SpoolEase-related subdirectory.

## Core Constraint

Keep the integration isolated in separate SpoolEase files/folder/module. Original Bambu files should receive only small hook calls, ideally one-liners, passing UI context into SpoolEase.
Critical guideline, above anything else: Minimal modifications should be made to the original project files. Only what is absolutely necessary. Architecture and code decisions should put this at a high priority even at the cost of architecture.

SpoolEase should be a separate module of its own, not placed inside an existing Bambu model or feature folder. It will eventually serve multiple integration points across the source tree, so it should not be nested under a specific GUI subsystem or a filament-manager subsystem.

Preferred pattern:

```cpp
SpoolEase::SomeUiHook(...context...);
```

The SpoolEase module should own lookup, formatting, tooltip text, and custom drawing where possible. If upstream UI is custom-painted and cannot be modified externally, hook the draw path with a minimal call and let SpoolEase draw the overlay.

## Printer Identity

Use `MachineObject::get_dev_id()` as the printer identifier passed to SpoolEase. In Bambu Studio this is the primary device id populated from JSON `dev_id`, used as the key in `DeviceManager`, and used for Bambu network/MQTT calls. It appears to be the printer serial/device ID.

Recommended abstraction:

```cpp
SpoolEase::printer_serial_for(obj)
```

Internally this can return `obj->get_dev_id()` now, while keeping the SpoolEase-facing API semantically named `printer_serial`.

Slot lookup context should be normalized as:

- `printer_serial`
- `ams_id`
- `slot_id`
- external slot marker/type where applicable

## Existing Code Areas Found

### Device Tab AMS / External Slots

Primary native wx/C++ path:

- `src/slic3r/GUI/StatusPanel.cpp`
- `src/slic3r/GUI/Widgets/AMSControl.cpp`
- `src/slic3r/GUI/Widgets/AMSItem.hpp`
- `src/slic3r/GUI/Widgets/AMSItem.cpp`

Important structures/functions:

- `AMSinfo::parse_ams_info(MachineObject* obj, DevAms* ams, ...)` builds AMS slot `Caninfo` objects.
- `AMSinfo::parse_ext_info(MachineObject* obj, DevAmsTray tray)` builds external slot info.
- `AmsItem::AddCan(...)` and `AmsItem::AddLiteCan(...)` create `AMSLib` slot widgets.
- `AmsItem::Update(...)` updates existing `AMSLib` slot widgets.
- `AMSLib::Update(Caninfo info, std::string ams_idx, bool refresh)` receives `ams_id` and `slot_id` via `info.can_id`.
- `AMSLib::render_generic_text(...)` currently builds the slot tooltip and draws material text.
- `AMSLib::render_generic_lib(...)` draws the actual colored slot body/remain area.

Existing tooltip behavior:

- Generic AMS slots currently set tooltip in `AMSLib::render_generic_text()` using material type and optional K value.

Minimal hook technique:

- Add a SpoolEase call after the native tooltip text is assembled in `AMSLib::render_generic_text()` so SpoolEase can append/replace tooltip text using `m_obj`, `m_ams_id`, `m_slot_id`.
- Add a SpoolEase draw call near the end of `AMSLib::render_generic_lib()` and equivalent Lite path if needed, after Bambu draws the slot body, so SpoolEase can overlay Spool ID/net weight at the top of the slot.
- Avoid modifying `Caninfo` unless absolutely necessary. `AMSLib` already has `m_obj`, `m_ams_id`, and `m_slot_id` context.

Likely hook shapes:

```cpp
tooltip_text = SpoolEase::Ui::decorate_ams_slot_tooltip(
    tooltip_text, m_obj, m_ams_id, m_slot_id);
```

```cpp
SpoolEase::Ui::draw_ams_slot_overlay(dc, *this, m_obj, m_ams_id, m_slot_id);
```

Risk notes:

- Device tab slot UI is custom-painted, so overlay drawing probably needs a drawing hook.
- Do not rely on Bambu `material_remain`, tray `weight`, or existing local inventory fields for SpoolEase data.

### Project Filaments AMS Dropdown

Relevant files:

- `src/slic3r/GUI/PresetComboBoxes.cpp`
- `src/slic3r/GUI/PresetComboBoxes.hpp`
- `src/slic3r/GUI/Plater.cpp`

Important functions:

- `Sidebar::build_filament_ams_list(MachineObject* obj)` builds `preset_bundle->filament_ams_list` entries with `ams_id`, `slot_id`, `tag_uid`, `tray_name`, filament metadata, etc.
- `PresetComboBox::add_ams_filaments(std::string selected, bool alias_name)` appends the `AMS filaments` section in the filament dropdown.
- `PresetComboBox::selected_ams_filament()` maps selected dropdown entry back to the `filament_ams_list` key.
- `PresetComboBox::update_selection()` sets the combo tooltip to the selected item string.

Important selection risk:

- Visible dropdown text is used for preset matching and selection via `GetString(...)` in several paths.
- Changing the visible label from the preset name to include Spool ID/net weight may break selection semantics.
- Breaking that functionality is not acceptable and solution should be found for this, or this will not be implemented

Minimal hook technique:

- In `PresetComboBox::add_ams_filaments()`, after `Append(text, bmp.ConvertToImage(), ...)`, call SpoolEase with tray context from the `DynamicPrintConfig` entry and set item tooltip.
- If visible text must include SpoolEase info later, it should be done with extreme care or by owner-drawn metadata that does not alter the logical preset string.

Likely hook shape:

```cpp
SpoolEase::Ui::decorate_ams_filament_dropdown_item(
    *this, item_id, tray);
```

Where `tray` contains Bambu context keys `ams_id`, `slot_id`, and `tray_name`; SpoolEase uses those to fetch external data.

Risk notes:

- Bambu stores AMS dropdown entries in `filament_ams_list` as `DynamicPrintConfig`; do not treat these values as inventory source of truth.
- The dropdown section exists only for Bambu vendor-compatible printer presets and non-empty `filament_ams_list`.

### Print Dialog Material/Slot Selection

Relevant files:

- `src/slic3r/GUI/SelectMachine.cpp`
- `src/slic3r/GUI/SelectMachine.hpp`
- `src/slic3r/GUI/AmsMappingPopup.hpp`
- `src/slic3r/GUI/AmsMappingPopup.cpp`
- `src/slic3r/GUI/AmsMappingPopupUpdate.cpp`

Important classes/structures:

- `MaterialItem`: project material tile above slot selection. Shows original material and selected AMS slot.
- `MappingItem`: individual selectable AMS/external slot in mapping popup.
- `TrayData`: popup slot data with `ams_id`, `slot_id`, `name`, `filament_type`, `remain`, color info.
- `FilamentInfo`: print/material mapping info from `src/libslic3r/ProjectTask.hpp`, includes `id`, `used_g`, `ams_id`, `slot_id`, `tray_id`, material type/color.

Important functions:

- `SelectMachineDialog::sync_ams_mapping_result(...)` updates `MaterialItem` selected AMS info.
- `SelectMachineDialog::update_print_required_data(...)` stores plate/config data.
- Print-dialog material item creation appears around `SelectMachine.cpp` lines near `4527` and `5043` depending flow.
- `SelectMachineDialog::CheckWarningFilamentRemain(...)` already computes required filament grams from G-code volumes and filament density.
- `AmsMapingPopup::update_mapping_items(...)` builds slot mapping UI from current machine data.
- `AmsMapingPopup::add_ams_mapping(...)` creates each `MappingItem`.
- `AmsMapingPopup::add_ext_ams_mapping(...)` handles external slot mapping.
- `MappingItem::set_data(...)` sets display data and tooltip for each selectable slot.
- `MaterialItem::set_ams_info(...)` updates selected slot display.
- `MaterialItem::doRender(...)` custom-draws material tile content.

Required filament amount:

- This is the one business value that should come from Bambu Studio.
- `FilamentInfo::used_g` exists.
- Existing code in `CheckWarningFilamentRemain()` computes used grams as:

```cpp
used_g = densities[fila.id] * (volumes_map[fila.id] / 1000);
```

Where `volumes_map` comes from:

```cpp
gcode_result->print_statistics.total_volumes_per_extruder
```

Minimal hook technique for required material weight:

- Add a SpoolEase call after each `MaterialItem` is created, passing the material logic id and required grams.
- SpoolEase should render/format the required-weight label or set a field on `MaterialItem` through a minimal API.
- If `MaterialItem` is custom-painted and no normal child label can be attached cleanly, add a drawing hook in `MaterialItem::doRender()`.

Likely hook shapes:

```cpp
SpoolEase::Ui::set_material_required_weight(*item, required_g);
```

or custom paint:

```cpp
SpoolEase::Ui::draw_material_required_weight(dc, *this, m_required_g);
```

Minimal hook technique for selected slot display:

- Hook `MaterialItem::set_ams_info(...)` or the callers in `sync_ams_mapping_result(...)` after Bambu sets selected AMS slot.
- Pass `printer_serial`, `ams_id`, `slot_id`, material id, and optionally required grams.
- SpoolEase can update tooltip and/or draw overlay with Spool ID/net weight.

Minimal hook technique for mapping popup slots:

- In `AmsMapingPopup::add_ams_mapping(...)`, before/after `m_mapping_item->set_data(...)`, pass popup slot context to SpoolEase.
- In `MappingItem::set_data(...)`, after native tooltip decision, call SpoolEase to decorate/replace the tooltip.
- If visible Spool ID/net weight must be drawn inside the slot tile, add a draw hook at the end of `MappingItem::doRender(...)`.

Likely hook shapes:

```cpp
SpoolEase::Ui::decorate_mapping_item_tooltip(
    *this, existing_tooltip, printer_serial, m_tray_data.ams_id, m_tray_data.slot_id);
```

```cpp
SpoolEase::Ui::draw_mapping_item_overlay(dc, *this, printer_serial, m_tray_data.ams_id, m_tray_data.slot_id);
```

Risk notes:

- `AmsMappingPopup` is reused by `SelectMachineDialog`, `SyncAmsInfoDialog`, and `SendMultiMachinePage`. Shared popup hooks can cover multiple flows but may need to distinguish print dialog vs sync dialog.
- The current `MaterialItem` tooltip contains generic instructions: “Upper half area...” and should be augmented/replaced with SpoolEase info in print dialog.

## Secondary Paths Found

### SyncAmsInfoDialog

Files:

- `src/slic3r/GUI/SyncAmsInfoDialog.cpp`
- `src/slic3r/GUI/SyncAmsInfoDialog.hpp`

This dialog also uses `AmsMapingPopup`, `MaterialItem`, and `FilamentInfo`. Shared `AmsMapingPopup`/`MaterialItem` hooks may affect it. Decide whether SpoolEase UI should appear there or whether hooks should be gated to print-dialog usage.

### SendMultiMachinePage

Files:

- `src/slic3r/GUI/SendMultiMachinePage.cpp`
- `src/slic3r/GUI/SendMultiMachinePage.hpp`

This page also uses `AmsMapingPopup` and material mapping data. Shared popup hooks may affect it. Decide whether that is desired.

### SendToPrinter

Files:

- `src/slic3r/GUI/SendToPrinter.cpp`
- `src/slic3r/GUI/SendToPrinter.hpp`

This appears to be a “send to printer storage” dialog and not the primary AMS material mapping print dialog. No primary SpoolEase hook identified here so far.

## Proposed SpoolEase Module Shape

Recommended placement:

```text
src/spoolease/
```

or, if project naming conventions prefer PascalCase:

```text
src/SpoolEase/
```

Avoid placing SpoolEase under `src/slic3r/GUI/` unless there is a concrete build-system constraint. The current hooks start in GUI code, but SpoolEase is conceptually broader than GUI and should remain as independent as possible.

Suggested internal split:

- Core/cache/API client code in `src/spoolease/` without Bambu UI dependencies where possible.
- A thin UI adapter layer in `src/spoolease/ui/` that can include wx/Bambu GUI types only for hook implementations.
- Public headers that Bambu source files include should be narrow and stable, such as `src/spoolease/SpoolEaseUiHooks.hpp`.

Build integration should also preserve separation. Prefer one small build hook that includes the SpoolEase module, while keeping the SpoolEase source list inside the SpoolEase folder. On this branch, likely options are:

- Add `add_subdirectory(../spoolease ${CMAKE_CURRENT_BINARY_DIR}/spoolease)` from `src/slic3r/CMakeLists.txt` if using a separate `src/spoolease/CMakeLists.txt`.
- Or append a minimal SpoolEase source list to `SLIC3R_GUI_SOURCES`, but keep that list maintained in the SpoolEase module if possible.
- Current branch build context: `src/slic3r/CMakeLists.txt` already includes `GUI/fila_manager` and `GUI/DeviceWeb`; `src/slic3r/GUI/DeviceWeb/CMakeLists.txt` builds the React `device_page` app and copies it to `resources/web/device_page/dist`.

The exact CMake approach can be chosen during implementation; the design goal is that original Bambu files only contain minimal hooks.

Recommended public API concepts:

```cpp
namespace SpoolEase::Ui {

struct SlotContext {
    std::string printer_serial;
    std::string ams_id;
    std::string slot_id;
    bool is_external = false;
};

std::string printer_serial_for(const MachineObject* obj);

wxString decorate_slot_tooltip(const wxString& existing, const SlotContext& ctx);

void draw_ams_slot_overlay(wxDC& dc, wxWindow& slot_window, const SlotContext& ctx);
void draw_mapping_item_overlay(wxDC& dc, wxWindow& item_window, const SlotContext& ctx);

void decorate_dropdown_item(PresetComboBox& combo, int item_id, const SlotContext& ctx);

void set_material_required_weight(MaterialItem& item, double required_g);
void draw_material_required_weight(wxDC& dc, MaterialItem& item, double required_g);

}
```

Implementation details can change, but Bambu source hooks should remain stable and thin.

## Data Source Rules

SpoolEase data must come from external SpoolEase cache/API, not Bambu Studio.

Allowed Bambu input:

- `MachineObject*` only to identify selected/current printer.
- AMS id and slot id from Bambu UI objects.
- External slot identity from Bambu UI objects.
- Required filament grams in print dialog.

Not allowed as SpoolEase source of truth:

- `DevAmsTray::remain`.
- `DevAmsTray::weight`.
- `DevAmsTray::get_filament_remain_weight()`.
- Any existing or future local Bambu/fork inventory fields like `net_weight`, `spool_id`, etc.
- Existing Bambu AMS RFID/tag fields except possibly as optional context/debug, not primary lookup.

## Current Branch `fila_manager` / DeviceWeb Findings

This branch contains a `src/slic3r/GUI/fila_manager/` module with local spool storage, AMS sync, and Bambu cloud sync/client code. It exposes fields such as `spool_id`, `net_weight`, `tag_uid`, `setting_id`, `bound_dev_id`, and `bound_ams_id`.

Relevant current files:

- `src/slic3r/GUI/fila_manager/wgtFilaManagerStore.h`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerStore.cpp`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerSync.h`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerSync.cpp`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudClient.h`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudClient.cpp`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudSync.h`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudSync.cpp`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudDispatcher.h`
- `src/slic3r/GUI/fila_manager/wgtFilaManagerCloudDispatcher.cpp`
- `src/slic3r/GUI/DeviceWeb/FilaManagerVM.hpp`
- `src/slic3r/GUI/DeviceWeb/FilaManagerVM.cpp`
- `src/slic3r/GUI/DeviceWeb/device_page/src/features/filament/`

Observed data/storage behavior:

- `wgtFilaManagerStore::get_storage_path()` stores local data at `data_dir()/filament_inventory/spools.json`.
- `FilamentSpool` includes `spool_id`, `setting_id`, `tag_uid`, `brand`, `material_type`, `series`, color fields, `initial_weight`, `spool_weight`, `remain_percent`, `status`, `entry_method`, `bound_dev_id`, `bound_ams_id`, `note`, `favorite`, `net_weight`, and `cloud_synced`.
- `wgtFilaManagerSync::on_device_update(MachineObject* obj)` is called from Bambu cloud/LAN device message paths in `GUI_App.cpp` and syncs AMS tray changes into the local store.
- `wgtFilaManagerCloudClient` uses Bambu `NetworkAgent` filament APIs such as create/update/delete/list filament spool and filament config.
- `wgtFilaManagerCloudDispatcher` serializes pull/create/update/delete cloud operations and emits sync state callbacks.
- `FilaManagerVM` is the active bridge facade for the React Filament Manager page. It handles `filament` module commands for `init`, `spool`, `preset`, `machine`, `ams`, `sync`, `config`, and `colors`.
- `FilaManagerVM::build_spool_list()` returns an empty list when the Bambu user is not logged in; otherwise it returns `fila_manager_store()->spools_to_json()` with color-name enrichment.
- The active React Filament Manager page is under `src/slic3r/GUI/DeviceWeb/device_page/src/features/filament/`, with bridge logic in `useFilamentBridge.ts` and types in `types.ts`.

Source-of-truth note:

The current `fila_manager` module is Bambu/local inventory and Bambu-cloud oriented. It should not be treated as SpoolEase inventory data. SpoolEase inventory data should still come from the external SpoolEase system, not from Bambu/local inventory fields.

## Suggested Implementation Order

1. Add the isolated SpoolEase UI module with no-op implementations first.
2. Hook Device tab AMS slot tooltip and overlay with one-line calls.
3. Hook print dialog mapping popup slot tooltip/overlay.
4. Hook print dialog material required grams using Bambu’s computed required amount.
5. Hook Project AMS dropdown item tooltip.
6. Only after tooltips/overlays are stable, consider visible text changes in dropdown if still required.

## Verification Notes

After implementation, verify at least:

- Project builds/compiles for touched files.
- Device tab still loads AMS and external slot UI.
- Filament dropdown selection still selects the correct preset and AMS source.
- Print dialog mapping still selects the correct AMS/external slot.
- Print required grams are correct for each material/color.
- Tooltips do not regress existing disabled/unmatched slot messages.
