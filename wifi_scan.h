#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Scanner — Phase 4 reference port
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 4)
//
// Reference implementation of the plan's feature lifecycle (§4.3): a
// radio_worker task on core 0 runs an async WiFi.scanNetworks(), posts a
// terminal RenderMsg{kind=RENDER_MSG_SCAN_COMPLETE} on telemetry_q when
// done (plan §5.1 migration mapping: "*ScanDone booleans -> a terminal
// RenderMsg on telemetry_q"), and blocks on a per-feature cmd_q waiting for
// CMD_STOP. The orchestrator (board_bringup_t_embed.cpp) owns navigation
// and joins the worker via a completion semaphore when BACK is pressed —
// this is the template later modules (wifi_attacks proper, subghz_attacks,
// etc.) copy, not a generic dispatcher (only one module uses this shape so
// far; extract a shared helper once a third module needs it).
// ═══════════════════════════════════════════════════════════════════════════

// Starts the scan worker on core 0. Call once per scan.
void wifiScanStart();

// Call from the orchestrator when BACK is pressed while a scan is running
// or its results are on screen. Blocks until the worker has actually
// exited (radio released) — the "cleanly joins the worker" exit test.
void wifiScanStop();

// Result accessors, valid after the RENDER_MSG_SCAN_COMPLETE telemetry
// message — formatted as plain strings so they can be handed straight to
// uiRenderShowSubmenu() (reusing the existing list renderer).
const char *const *wifiScanGetResultLines();
int wifiScanGetResultCount();

// Raw per-result BSSID/channel, valid over that same window (the scan
// results aren't torn down until wifiScanStop() is called) — feeds
// wifi_attacks.cpp's WifiScan "tap-to-attack" handoff into Deauther
// (board_bringup_t_embed.cpp's NAV_WIFI_SCAN_RESULTS SELECT handler calls
// wifiScanStop() then deautherSetTarget() with these). Returns a channel
// of 0 for the "(no networks found)" placeholder row — callers should
// treat that as "nothing to hand off".
const uint8_t *wifiScanGetResultBSSID(int index);
const char *wifiScanGetResultSSID(int index);
uint8_t wifiScanGetResultChannel(int index);

#endif // WIFI_SCAN_H
