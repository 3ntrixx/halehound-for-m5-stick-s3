#ifndef AUTH_FLOOD_H
#define AUTH_FLOOD_H

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Auth Flood — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A full-fidelity port of wifi_attacks.cpp's AuthFlood namespace's core
// attack: scan -> pick a target AP -> continuous 802.11 Authentication-
// Request flood against it (same 30-byte frame template — random source
// MAC + random sequence number per packet — same raw ESP-IDF APSTA +
// hidden-AP TX path as Deauther/BeaconSpammer).
//
// Structurally identical to deauther.cpp's own SCAN -> AP_LIST -> ATTACK
// shape (this project's own established pattern), reused directly here —
// selecting an AP starts the attack immediately, collapsing the original's
// separate "target selected, tap START to begin" standby screen, same
// simplification already applied to Deauther.
//
// Deliberately NOT ported: touch-driven paging (encoder scroll instead),
// the 85-bar equalizer visual, and the original's own tap-to-rescan icon
// (BACK already re-scans from the AP list, same as every other list here).
// Unlike Deauther, the original has no WifiScan tap-to-attack handoff for
// AuthFlood, so none is added here either.
// ═══════════════════════════════════════════════════════════════════════════

void authFloodScanStart();
void authFloodScanStop();

const char *const *authFloodGetApResultLines();
int authFloodGetApResultCount();

void authFloodSelectTarget(int index);
void authFloodAttackStop();

#endif // AUTH_FLOOD_H
