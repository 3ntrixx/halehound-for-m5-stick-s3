#ifndef LUNATIC_FRINGE_H
#define LUNATIC_FRINGE_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Lunatic Fringe — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A full-fidelity port of bluetooth_attacks.cpp's LunaticFringe namespace:
// a continuous passive BLE scan classifying advertisements against five
// known tracker fingerprints — Google FMDN (service data UUID 0xFEAA,
// frame type 0x40/0x41), Samsung SmartTag (0xFD5A, byte0&0xF8==0x10), Tile
// (0xFEED, bytes 0x02 0x00), Chipolo (0xFE33, any service data), and Apple
// AirTag (manufacturer ID 0x004C, type 0x12, len 0x19) — same UUIDs/byte
// offsets as the original, ported verbatim. Devices are deduped by MAC and
// updated in place on repeat sightings (RSSI/lastSeen), same as the
// original's addOrUpdateDevice(); reuses ble_scan.cpp's RUNNING->RESULTS
// worker shape (radio_worker on core 0 acquires RADIO_BLE, runs a chained
// async BLEScan via a completion callback, keeps live-updating the result
// list on RENDER_MSG_SCAN_COMPLETE while it's on screen — the list is
// never a static one-shot snapshot).
//
// UI: screen-for-screen port of the reference build's own LUNATIC FRINGE
// screen (ui_render.h's uiRenderShowTrackerScanning()/ShowTrackerList()) —
// glitch title, FOUND/SCANS counters, a signal-bar row per device (type
// code, short MAC, RSSI, the same 5-tier proximity estimate the original
// calls tdProximityStr(), and a live/active dot). The original's separate
// tap-for-detail overlay (status byte, first/last-seen timers, a large
// proximity bar) is NOT ported — this board has no touchscreen/drill-down
// input for it.
// ═══════════════════════════════════════════════════════════════════════════

#include "ui_render.h" // TrackerDisplayRow / UI_TRACKER_MAX_ROWS

// Starts the scan worker on core 0. Call once per entry into the feature.
void lunaticFringeStart();

// Call from the orchestrator when BACK is pressed while scanning or
// browsing results. Blocks until the worker has actually exited (radio
// released, BLE controller deinitialized).
void lunaticFringeStop();

// Result accessors, valid after the first RENDER_MSG_SCAN_COMPLETE and
// refreshed live on every subsequent one while results are on screen.
int lunaticFringeGetDeviceCount();  // raw count, 0 when nothing found yet
int lunaticFringeGetScanCount();    // completed 5s scan windows this session
int lunaticFringeGetDisplayRows(TrackerDisplayRow *out, int max);

#endif // LUNATIC_FRINGE_H
