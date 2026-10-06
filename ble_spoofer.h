#ifndef BLE_SPOOFER_H
#define BLE_SPOOFER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Spoofer — on-chip ESP32-S3 BLE (Cardputer ADV + T-Embed CC1101)
// Created: 2026-09-21 (Cardputer ADV port — on-chip BLE attacks)
// Added to the T-Embed CC1101 build 2026-09-22 (Bluetooth submenu "BLE
// Spoofer" row) — this file is genuinely board-agnostic (messaging.h/
// radio_manager.h only, no board-specific pins or display calls), so it
// ports to the encoder-input T-Embed unchanged from the keyboard-input
// Cardputer ADV; only each board's own orchestrator wiring differs. Kept
// Bluedroid rather than NimBLE for this port — see the port history for the
// tradeoff discussion (every other T-Embed BLE module — ble_scan.cpp,
// lunatic_fringe.cpp, ble_skeletonkey.cpp — is already Bluedroid-based, and
// the two host stacks can't link into one firmware image).
//
// Port of bluetooth_attacks.cpp's BleSpoofer namespace (~lines 55-1410): the
// Apple/Beats "AirPods popup spam" engine, generalized in that source into a
// 7-mode multi-platform spam engine (Apple Proximity Pairing, Apple Nearby
// Action, Google Fast Pair, Samsung Buds EasySetup, Samsung Watch pairing,
// Microsoft Swift Pair, and a CHAOS mode that round-robins all of the above).
// Both boards have on-chip ESP32-S3 BLE only (no NRF24/CC1101 involvement),
// which is exactly what this feature already used — the original's raw
// ESP-IDF GAP sequence (stop advertising -> set random MAC -> configure raw
// adv data -> start advertising -> ~20ms -> stop, repeat) is standard
// Bluedroid controller behavior shared by the classic ESP32 and ESP32-S3
// alike, so that broadcast core, all 6 payload-generator functions and every
// device signature table (20 Apple/Beats models, 14 Nearby Action types, 15
// Fast Pair models, 20 Samsung Buds IDs, 25 Samsung Watch IDs, 10 Swift Pair
// names) are ported byte-for-byte unchanged. Max TX power
// (ESP_PWR_LVL_P9) and the Classic-BT-memory-release-before-init trick are
// also ported verbatim (same helper shape as lunatic_fringe.cpp/
// ble_scan.cpp's releaseClassicBtMemory()).
//
// Structurally this follows ble_jammer.h/.cpp: a single FreeRTOS worker task
// on core 0 (messaging.h's Cmd-queue stop instead of the original's
// spamming-bool-plus-join), radio_manager's RADIO_BLE acquire/release
// bracketing the whole run (like lunatic_fringe.cpp/ble_scan.cpp), and
// RENDER_MSG_STATUS_TEXT telemetry posts instead of direct TFT drawing —
// no getter function, matching ble_jammer.cpp's pattern exactly since this
// is a running counter/status feature, not a result list.
//
// Dropped/changed from the CYD original (touch UI; neither board here has
// an equivalent control surface):
//   - All direct TFT drawing (icon bar, glitch-text header, scrolling
//     broadcast log, activity equalizer, skull row animation, gradient
//     counter bar) — replaced by one status line via uiRenderShowStatus()/
//     showFeatureStatus(), same as every other running feature on both
//     boards.
//   - The CYD's 6-icon touch bar (Back/Toggle/PrevMode/NextMode/PrevDevice/
//     NextDevice) collapses to Start/Stop/AdjustMode(delta) — AdjustMode
//     cycles the 7 platform modes, the same single axis ble_jammer.h's
//     rotate hook exposes for its 4 channel modes (keyboard delta on
//     Cardputer ADV, encoder rotate on T-Embed — this file doesn't care
//     which).
//   - Manual per-device Prev/Next selection is dropped: with only one
//     encoder-like axis available (no second control for device-within-
//     mode), each broadcast now automatically round-robins to the next
//     device in the active platform's table instead of sitting on a
//     manually-picked device until touched. Net effect is *more* spam
//     variety per mode, not less — every AirPods/Beats/Galaxy Buds/etc.
//     variant in the active mode gets hit in turn automatically. CHAOS
//     mode's own round-robin-platform + random-device-within-platform
//     logic is unchanged (it already picked devices automatically).
//   - setInitialMode()/the Sour-Apple quick-launch redirect is dropped —
//     it only existed so a separate CYD menu entry could jump straight into
//     the Sour Apple sub-mode; no such menu wiring exists on this board and
//     AdjustMode already reaches every mode directly.
//   - EEPROM persistence of the selected mode: not ported (no other running
//     feature on this board persists its mode across power cycles either).
// ═══════════════════════════════════════════════════════════════════════════

// Starts the spam worker on core 0 (acquires RADIO_BLE). Call once per
// entry into the feature.
void bleSpooferStart();

// Call from the orchestrator when BACK is pressed. Blocks until the worker
// has actually exited (advertising stopped, BLE controller deinitialized,
// radio released).
void bleSpooferStop();

// Keyboard/encoder-equivalent input while running: delta +1 = next platform
// mode, -1 = previous, wrapping through all 7 (Apple Popup, Sour Apple,
// Fast Pair, Samsung Buds, Samsung Watch, Swift Pair, CHAOS). Live — no
// radio reinit needed, same as bleJammerAdjustMode(). Mode selection
// persists across Stop()/Start() within a session, also matching
// ble_jammer.cpp.
void bleSpooferAdjustMode(int delta);

#endif // BLE_SPOOFER_H
