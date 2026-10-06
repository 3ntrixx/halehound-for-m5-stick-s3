#ifndef BLE_SCAN_H
#define BLE_SCAN_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Scanner — Phase 5 reference port (RF breadth: BLE first)
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 5)
//
// Implements BlePredator's "Phase 1: LISTEN" behavior from
// bluetooth_attacks.cpp — continuous passive scan (5s windows auto-chained),
// dedup by MAC with RSSI/min/max + frame-count + first/last-seen tracking,
// name/manufacturer/raw-payload merged in as later frames carry it, RED/
// YELLOW/SKIP tier classification from ble_database.h's tables (SKIP always
// hidden), and the ALL/NAMED/STRONG/HIGH filter modes. Runs as a
// radio_worker task on core 0 that acquires RADIO_BLE, using the same
// feature lifecycle as wifi_scan.cpp (plan §4.3).
//
// BlePredator's Phase 2 (target detail overlay) and Phase 3 (raw MAC/
// payload replay attack) are NOT implemented here — deferred along with
// the rest of bluetooth_attacks.cpp's deep touch/graphics coupling.
//
// Validates the plan's Bluedroid-on-S3 open question (§7 risk table:
// "Bluedroid RAM pressure on S3 alongside WiFi + PSRAM sprite") on real
// hardware before any of the other BLE modules are ported.
// ═══════════════════════════════════════════════════════════════════════════

#include "ui_render.h" // BleDisplayRow / BleDisplayStats / UI_BLE_MAX_ROWS

// Starts the scan worker on core 0. Call once per scan.
void bleScanStart();

// Call from the orchestrator when BACK is pressed while a scan is running
// or its results are on screen. Blocks until the worker has actually
// exited (radio released, BLE controller deinitialized).
void bleScanStop();

// Result accessors, valid after the RENDER_MSG_SCAN_COMPLETE telemetry
// message — formatted as plain strings so they can be handed straight to
// uiRenderShowSubmenu() (reusing the existing list renderer). Only devices
// passing the current filter are included; SKIP-tier devices never are.
const char *const *bleScanGetResultLines();
int bleScanGetResultCount();

// Current filter mode name ("ALL"/"NAMED"/"STRONG"/"HIGH") for the list
// title — same modes/semantics as BlePredator's PredFilter.
const char *bleScanGetFilterName();

// RED / YELLOW tier counts across all tracked devices (SKIP excluded) for
// the list header — matches the original's stats line.
void bleScanGetTierCounts(int *red, int *yellow);

// Cycles ALL -> NAMED -> STRONG -> HIGH -> ALL (the encoder equivalent of
// the original's filter touch button). The list refreshes on the worker's
// next pass (~1s).
void bleScanCycleFilter();

// Copies the current filtered device list + stats into `out`/`stats` for
// uiRenderShowBleDevices() — the full Predator screen (RSSI bars, DEVICE/
// WHAT columns, tier colors, stats line, page footer).
void bleScanGetDisplayRows(BleDisplayRow *out, int max, BleDisplayStats *stats);

// Raw 6-byte MAC + address type for filtered-list index `index` (same
// indexing as bleScanGetDisplayRows()/GetResultLines()) — BleDisplayRow
// only carries cosmetic text, not what BLEClient::connect() actually
// needs. Added for SkeletonKey's Predator-handoff (v3.9.x port plan
// Phase 11). Returns false if index is out of range.
bool bleScanGetTargetMac(int index, uint8_t *outMac, uint8_t *outAddrType);

// The device's real advertised name for filtered-list index `index` (same
// indexing as bleScanGetTargetMac()), or false if it never advertised one
// (BleDisplayRow.device falls back to a MAC fragment in that case, which
// is not a usable name). Added alongside bleScanGetTargetMac() for
// SkeletonKey's Predator-handoff: without the real name, SkeletonKeyProtocol
// ::detect() can never match a profile by name (it had nothing but a MAC
// string to compare against — see skeletonKeyStart()'s `name` parameter).
bool bleScanGetTargetName(int index, char *outName, size_t outSize);

#endif // BLE_SCAN_H
