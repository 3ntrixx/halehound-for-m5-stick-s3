#ifndef DEAUTHER_H
#define DEAUTHER_H

#include <stdint.h>

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Deauther — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 phase 6 continued)
//
// A full-fidelity port of wifi_attacks.cpp's Deauther namespace's core
// attack: scan -> pick a target AP -> continuous broadcast deauth against
// it (same 26-byte frame template, same raw ESP-IDF APSTA + hidden-AP TX
// path, same packetsPerBurst=10 tight-loop TX task on its own core with
// heap-safety abort and consecutive-failure WiFi restart). Per user
// direction ("reproduce anything halehound does... as much as you possibly
// can"), following the same SCAN -> AP_LIST -> ATTACK shape already
// established by eapol_sniff.cpp (this file's scan phase is a near-verbatim
// reuse of that one's — same APInfo struct, same async-scan-poll pattern).
//
// Deliberately NOT ported (out of scope for this reference port, matching
// this project's precedent of trimming touch-UI-only chrome): the
// touch-driven networksPerPage list paging (mapped here to encoder
// scroll instead, like every other result list in this project),
// wardriving-toggle-while-attacking, the packets-per-burst on-device
// tuning control (fixed at the original's own default of 10), and
// per-station (vs. broadcast) targeting (StationScan's own namespace,
// unported) — the broadcast deauth (receiver = FF:FF:FF:FF:FF:FF) IS the
// original's own default behavior, so nothing is lost there.
// ═══════════════════════════════════════════════════════════════════════════

// Starts the AP scan phase. Call from the orchestrator on menu SELECT.
void deautherScanStart();

// Call from the orchestrator when BACK is pressed during SCAN/AP_LIST.
// Blocks (bounded) until the scan worker has exited.
void deautherScanStop();

const char *const *deautherGetApResultLines();
int deautherGetApResultCount();

// Stops the (still-alive, parked) scan worker and starts the continuous
// deauth attack against apList[index] — mirrors eapol_sniff.h's
// eapolSniffSelectTarget()'s same "join the scan worker first" contract.
void deautherSelectTarget(int index);

// Starts the attack directly against a caller-supplied target, skipping
// Deauther's own scan/AP-list phase entirely — ports wifi_attacks.cpp's
// WifiScan "tap-to-attack" handoff (Deauther::setTarget()). The caller must
// NOT have a scan/attack of its own already holding RADIO_WIFI (release it
// first, e.g. via wifiScanStop()) since this acquires it itself, same as
// deautherSelectTarget().
void deautherSetTarget(const uint8_t bssid[6], const char *ssid, uint8_t channel);

// Call from the orchestrator when BACK is pressed during the attack. Blocks
// (bounded) until both the TX task and the attack worker have exited.
void deautherAttackStop();

#endif // DEAUTHER_H
