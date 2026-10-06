#ifndef RADIO_MANAGER_H
#define RADIO_MANAGER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Radio Manager (thin)
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §6.3)
//
// Owns "which RF frontend is active" and enforces that only one is, at the
// mutex/bookkeeping level. This is the SAME modal invariant the CYD build
// already relies on informally (one feature live at a time, plan §1); this
// module just makes it a real lock instead of "by convention".
//
// Scope note: this phase only builds the acquire/release skeleton + mutual
// exclusion. Actually moving the scattered esp_wifi_*/esp_bt_controller_*
// enable/disable calls out of wifi_attacks.cpp/bluetooth_attacks.cpp and
// into radioAcquire()/radioRelease() is real per-module port work — that
// happens as each module is ported (plan §6.4 ledger), not here.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>

enum RadioKind {
    RADIO_NONE,
    RADIO_WIFI,
    RADIO_BLE,
    RADIO_CC1101,
    RADIO_NRF24,
};

// Call once from setup().
void radioManagerSetup();

// Attempts to make `kind` the active radio. Fails (returns false) if a
// different radio is already active — caller must radioRelease() first.
// Re-acquiring the already-active kind is a no-op success.
bool radioAcquire(RadioKind kind);

// Releases the currently active radio (only if it matches `kind`).
void radioRelease(RadioKind kind);

RadioKind radioGetActive();

#endif // RADIO_MANAGER_H
