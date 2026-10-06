# HaleHound T-Embed to M5StickS3: Wi-Fi and BLE port outline

Prepared against the current workspace on October 4, 2026. This is an implementation outline, not a completed or hardware-tested port.

**Recommendation:** add a separate StickS3 target, reuse the current T-Embed Wi-Fi/BLE feature implementations, and borrow the Cardputer port's compact display layouts and reduced orchestrator structure. Treat the current T-Embed implementation as the feature-parity baseline; the Cardputer port does not include every recent T-Embed feature.

1. **Define the target and feature boundary.**

   Assume M5Stack StickS3 (K150). It has an ESP32-S3, 8 MB flash, 8 MB octal PSRAM, a 135x240 ST7789P3 display, two application buttons, and M5PM1 power management. Use the display in 240x135 landscape. Plan for onboard storage without microSD. Hardware and pin references: [M5Stack StickS3 documentation](https://docs.m5stack.com/en/core/StickS3).

   The radio scope is 2.4 GHz Wi-Fi and Bluetooth LE. ESP32-S3 does not provide Bluetooth Classic or 5 GHz Wi-Fi. [Espressif radio specifications](https://www.espressif.com/en/products/socs/esp32-s3) and [Bluetooth support](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32s3/api-guides/bluetooth.html).

   Exclude CC1101/Sub-GHz, NRF24, RFID/NFC, their drivers, their initialization, and their menus. Also leave GPS, IR, audio, and IMU features outside this Wi-Fi/BLE project. Keep required support functions: display, input, power, settings, storage, export, and firmware flashing.

   Important exception: `ble_jammer.cpp` includes `nrf24_radio.h`. Exclude BLE Jammer even though it appears in the Bluetooth menu. Likewise exclude NRF-based WLAN Jammer and Proto Kill; their names do not make them onboard Wi-Fi features.

2. **Create an independent board target and prove basic hardware.**

   Add `env:m5stick-s3` in `platformio.ini`, `board_m5stick_s3.h`, and `board_bringup_m5stick_s3.cpp`. Route a new `BOARD_M5STICK_S3` flag through `cyd_config.h`. Use an explicit source allowlist so this target compiles exactly one `setup()`/`loop()` and never pulls in excluded radio modules.

   Configure actual 8 MB flash and octal PSRAM rather than copying T-Embed's 16 MB partition layout or Cardputer's no-PSRAM setting. Budget application space, settings, and a filesystem after measuring the linked firmware; decide OTA support from that budget.

   Keep the working T-Embed environment pinned. Its current platform is `espressif32@6.9.0`; M5Stack's StickS3 example uses `6.12.0`. Choose and pin a StickS3 toolchain/library combination after a bring-up build, then check existing Wi-Fi/BLE API compatibility. Do not combine the board port with an unplanned Arduino major-version migration. [Vendor PlatformIO example](https://docs.m5stack.com/en/core/StickS3).

   Start with USB serial, memory detection, display test, button events, and battery/USB operation. Use the M5PM1-supported initialization path for board power. Validate any available battery readings rather than copying Cardputer's ADC implementation or T-Embed's fuel-gauge driver.

   Milestone: repeatable boot on USB and battery, correct display colors/orientation, both buttons responsive, and verified flash/PSRAM sizes.

3. **Adapt display ownership and input to the existing architecture.**

   `ui_render.cpp` currently owns `TFT_eSPI` and a sprite. For minimum rendering churn, retain that path initially and add `User_Setup_M5StickS3.h`. The documented LCD pins are MOSI 39, SCLK 40, DC 45, CS 41, RESET 21, and BL 38. Application buttons are GPIO 11/12; internal I2C uses SDA 47/SCL 48. Verify panel offsets and inversion on hardware. [StickS3 pin map](https://docs.m5stack.com/en/core/StickS3).

   Keep one owner of display initialization. If using M5Unified for board support, configure or adapt display ownership so it and TFT_eSPI do not independently initialize/control the same panel. A later M5GFX conversion is a separate renderer change, not a prerequisite for porting the radio modules.

   Add `button_input.cpp/.h` and a button branch in `input_task.cpp`. Reuse `messaging.h`'s existing input queue. Suggested mapping:

   | Gesture | Existing input action |
   | --- | --- |
   | A short press | Next / `INPUT_EVENT_ENC_CW` |
   | B short press | Previous / `INPUT_EVENT_ENC_CCW` |
   | A long press | Select / `INPUT_EVENT_BTN_SHORT` |
   | B long press | Back or stop / `INPUT_EVENT_BACK` |
   | A+B chord | Secondary action / `INPUT_EVENT_BTN_LONG` |

   Suppress short-press events after a long press and suppress individual events after a chord. This preserves the fifth action used by filters, rescans, and feature-specific controls. Reserve the power/reset button for its hardware role.

   Borrow the Cardputer layout logic already in `ui_render.cpp`, but generalize it around display dimensions/capabilities. Audit every retained screen for clipped rows, columns, graphs, dialogs, and footer instructions. Avoid relying on the Cardputer-specific preprocessor branches being selected automatically.

   Milestone: all five logical actions work, lists scroll, detail screens remain readable, and every running feature has an accessible stop/back action.

4. **Separate reusable services from T-Embed hardware.**

   Reuse the queue contracts, settings, PSRAM pool, radio manager, and feature-worker start/stop pattern. Adapt `spi_manager.cpp`, `i2c_manager.cpp`, and `battery_gauge.cpp` only where board capabilities require it; there is no reason to initialize a shared external-radio/SD bus on this target.

   Build a StickS3 orchestrator from the smaller Cardputer structure, then bring over the missing T-Embed Wi-Fi/BLE states. Inventory the current T-Embed call paths as well as menu labels: comments describing earlier passive-only milestones are not a reliable record of current behavior.

   Use stable feature identifiers or a consistent board menu mapping. Removing categories from `menu_data.h` must not redirect old numeric menu indices to different handlers. Suggested top level: Wi-Fi, Bluetooth, Captures, Settings, About; place Wi-Fi Guardian and Wi-Fi/BLE SIGINT features under the relevant retained category.

   Milestone: the trimmed firmware builds without CC1101, NRF24, PN532, encoder, keyboard, or SD dependencies accidentally remaining in the selected code paths.

5. **Replace the SD storage assumption before promising feature parity.**

   `storage_manager.cpp` directly uses `SD` and shared-SPI selection. `saved_captures_tembed.cpp` and `loot_viewer_tembed.cpp` also access `SD` directly; changing only the storage manager is insufficient.

   Add a filesystem-neutral storage interface with a LittleFS backend for StickS3. Preserve queued writes and explicit completion/error reporting. Replace card-presence checks with storage-availability checks in retained modules. Keep settings in the existing settings backend.

   Use bounded PSRAM buffering, batched filesystem writes, file-size limits, and visible free-space/write-error reporting. Do not format automatically on mount failure or silently overwrite old captures. Provide list/export/delete operations over USB serial; an optional browser download mode can run after the radio feature stops.

   Audit all retained modules for SD paths, external data files, capture formats, and card-specific messages. Preserve existing file formats where possible. Treat `wardrive_lite.cpp` as Wi-Fi survey logging without coordinates unless GPS is separately added later.

   Milestone: save, reboot, browse, export, and delete a sample capture; full-storage and interrupted-write cases fail visibly without freezing the UI.

6. **Port features in staged groups.**

   | Stage | Existing modules/features | Main porting concern |
   | --- | --- | --- |
   | First usable build | Wi-Fi Scanner, Packet Monitor, BLE Predator scan | Radio lifecycle, small-screen result lists, responsive cancellation |
   | Observation tools | Station Scanner/OUI lookup, Probe Sniffer, Wi-Fi Guardian, SkeletonKey GATT browsing | Details, filters, secondary actions, BLE connect/disconnect cleanup |
   | Storage and combined tools | Wi-Fi survey logging, EAPOL Capture, Saved Captures, Loot viewer, IoT Recon, Flock You | Filesystem abstraction, complete current feature behavior, Wi-Fi/BLE handoffs |
   | Remaining Wi-Fi tools | Existing beacon, deauther, authentication, captive-portal, and Karma modules | Framework compatibility, existing behavior, two-button configuration, stop handling |
   | Remaining BLE tools | BLE Beacon, BLE Spoofer, Lunatic Fringe, WhisperPair, Airoha RACE | Existing BLE transport requirements, target selection, secondary actions, report storage |

   Relevant source names include `wifi_scan.cpp`, `packet_monitor.cpp`, `ble_scan.cpp`, `station_scan.cpp`, `oui_registry.cpp`, `probe_sniffer.cpp`, `wifi_guardian.cpp`, `ble_skeletonkey.cpp`, `wardrive_lite.cpp`, `eapol_sniff.cpp`, and the current `*_tembed.cpp` implementations.

   Keep the current BLE stack for the initial port; a simultaneous NimBLE migration would introduce another compatibility project. Review SDK-dependent raw Wi-Fi behavior separately and do not equate successful compilation with functional equivalence. For every module, record: included, builds, UI verified, radio behavior verified, storage/export verified, and remaining limitations.

   Feature names indicate scope, not a claim that every existing implementation is complete or automatically compatible. Verify transport and actual current functionality before marking parity achieved.

7. **Validate lifecycle, power, and regression behavior.**

   Test repeated start/stop cycles and Wi-Fi-to-BLE-to-Wi-Fi transitions. Confirm workers stop, callbacks are detached, queues drain appropriately, radio ownership is released, and internal heap/PSRAM do not steadily shrink. The current radio manager tracks ownership; individual modules still perform much of the hardware teardown.

   Exercise long result lists, empty scans, connection failures, allocation failures, storage-full conditions, long presses/chords, and back during active work. Confirm drawing and file writes do not block radio callbacks. Check battery operation, screen timeout/wake, and behavior after power loss.

   Rebuild the existing T-Embed and Cardputer environments after shared-code changes and run relevant existing host regressions. Test radio behavior with controlled test devices. Package a separately named StickS3 firmware image with documented flashing/recovery steps and the verified feature matrix.

   Completion means all included features have usable controls, clean exit paths, verified behavior on real StickS3 hardware, and working persistence/export where applicable. A successful build alone is not completion.

**Suggested implementation order:** board boot and power -> buttons and compact menus -> Wi-Fi/BLE scanning -> onboard storage/export -> remaining features -> repeated lifecycle and battery tests -> release package.

The largest expected work items are the two-button interaction model, SD removal across all consumers, and board/framework integration. The existing ESP32-S3 radio logic and Cardputer screen adaptations should reduce the amount of feature code that needs rewriting.
