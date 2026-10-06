#ifndef BLE_BEACON_H
#define BLE_BEACON_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Beacon — on-chip ESP32-S3 BLE (Cardputer ADV + T-Embed CC1101)
// Created: 2026-09-21 (Cardputer ADV port — on-chip ESP32-S3 BLE only)
// Added to the T-Embed CC1101 build 2026-09-22 (Bluetooth submenu "BLE
// Beacon" row) — board-agnostic as written (messaging.h/radio_manager.h
// only), so it ports unchanged; see ble_spoofer.h's header for the
// Bluedroid-vs-NimBLE tradeoff this project settled on for both files.
//
// Port of bluetooth_attacks.cpp's BleBeacon namespace (lines 1435-2265): the
// iBeacon/Eddystone advertisement broadcast engine. The packet builders
// (buildIBeaconPacket()/buildEddystoneURL()/buildEddystoneUID()), the UUID
// presets and the raw ESP-IDF advertising path (esp_ble_gap_config_adv_
// data_raw() + esp_ble_gap_start_advertising()/stop_advertising(), a fresh
// random static MAC each cycle, ADV_TYPE_NONCONN_IND / BLE_ADDR_TYPE_RANDOM
// / ADV_CHNL_ALL, ~21ms/cycle: delay(1) after loading the payload, delay(20)
// advertising, then stop) are ported byte-for-byte and call-for-call — this
// board's on-chip BLE controller runs the exact same raw GAP calls the CYD
// build used, so nothing about the RF path needed to change.
//
// Modes (same 6, same order, same wraparound cycling as the original's
// nextMode()/prevMode()):
//   0 RANDOM iBEACON — fresh random UUID/major/minor every packet
//   1 APPLE STORE    — real Apple Store retail UUID, randomized major/minor
//   2 STARBUCKS      — real Starbucks retail UUID, randomized major/minor
//   3 EDDYSTONE URL  — "google.com", Eddystone-URL frame w/ .com compression
//   4 EDDYSTONE UID  — random 10-byte namespace + 6-byte instance
//   5 GEO-FENCE      — cycles Apple/Starbucks/Target/Walmart/Macys UUIDs,
//                       one preset per packet (original's rapid-cycle mode)
//
// Not ported (this board's CYD-era touch UI has no equivalent): the touch
// icon bar (Back/Toggle/PrevMode/NextMode), direct TFT header/log/skull-row
// drawing, and the 8-line scrolling log. Status is now a single telemetry-
// driven line (messaging.h's RENDER_MSG_STATUS_TEXT via
// messagingPostTelemetry(), rendered through ui_render.h's
// uiRenderShowStatus()) carrying the current mode name, the per-packet
// UUID/major/minor (or URL/namespace) detail, and the running broadcast
// count — the same UI reduction ble_jammer.cpp and beacon_spammer.cpp
// already made for their own CYD-era screens. EEPROM mode persistence was
// never actually wired up in the original (currentMode always resets to
// BCN_RANDOM_IBEACON in setup()), so there was nothing to port there; this
// port keeps that same reset-on-start behavior — see bleBeaconStart().
//
// Starts broadcasting immediately on bleBeaconStart(), per this project's
// established convention (ble_jammer.cpp, beacon_spammer.cpp) rather than
// the original's separate touch-arm step. No on/off toggle control is
// exposed here — the menu's own Start/Stop already covers that, same as
// ble_jammer.h.
//
// Board-agnostic: on-chip ESP32-S3 BLE via the Arduino BLE stack
// (BLEDevice::init()/deinit() bracketing the same raw esp_ble_gap_*() calls
// the original used) through radio_manager.h's RADIO_BLE acquire/release —
// no CC1101/NRF24 hardware involved, unlike ble_jammer.cpp/beacon_spammer.cpp.
// ═══════════════════════════════════════════════════════════════════════════

void bleBeaconStart();
void bleBeaconStop();

// Encoder rotate while running: delta +1 (CW) = next mode, -1 (CCW) = prev,
// wrapping through all 6 modes. Live — no radio reinit needed, same idiom
// as ble_jammer.h's bleJammerAdjustMode()/beacon_spammer.h's
// beaconSpammerAdjustChannel().
void bleBeaconAdjustMode(int delta);

#endif // BLE_BEACON_H
