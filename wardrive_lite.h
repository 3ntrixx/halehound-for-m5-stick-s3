#ifndef WARDRIVE_LITE_H
#define WARDRIVE_LITE_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Wardrive Lite — Phase 6 reference port (roadmap §7 phase 6:
// "storage task, wardriving+GPS+eapol_capture on log_q/PSRAM. Exit test:
// sustained wardrive with no SD-induced packet loss.")
// Created: 2026-09-17 (T-Embed CC1101 port)
//
// A WiFi-only reimplementation of wardriving.cpp's core loop (WigleWifi-
// 1.6 CSV, same column layout) — NOT a GPS-tagged wardrive, because this
// board has no onboard GPS (board_t_embed_cc1101.h: CYD_HAS_GPS=0,
// GPS_RX_PIN/GPS_TX_PIN both -1, "wire one up via Grove/expansion" —
// nothing is connected right now). Every row's lat/lon/altitude/accuracy
// field is always 0.0/0.0/0.0/0, clearly different from a real wardrive
// log; porting gps_module.cpp (1330 lines) for untestable hardware would
// violate the same rule that kept wifi_attacks.cpp/bluetooth_attacks.cpp
// out of this port. GPS tagging is a straightforward follow-up once a
// module is actually wired to GPS_RX_PIN/GPS_TX_PIN.
//
// What this DOES validate for real, per the phase's actual exit test: a
// continuous WiFi scan loop logging every result through storage_manager
// (PSRAM staging + log_q + the dedicated storage task) without the radio
// loop ever blocking on SD — the new CC1101/SD-shared-bus risk from the
// plan's §7 risk table, exercised here via WiFi+SD instead since this
// module doesn't touch CC1101 at all.
// ═══════════════════════════════════════════════════════════════════════════

// Starts the continuous scan+log worker on core 0.
void wardriveLiteStart();

// Call from the orchestrator when BACK is pressed. Blocks until the
// worker has actually exited (radio released).
void wardriveLiteStop();

#endif // WARDRIVE_LITE_H
