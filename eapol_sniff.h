#ifndef EAPOL_SNIFF_H
#define EAPOL_SNIFF_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound EAPOL Sniffer — full-fidelity reimplementation of
// eapol_capture.cpp's EapolCapture namespace (target selection, deauth
// injection, on-device PMKID/handshake extraction, PCAP + .hc22000
// output) adapted to encoder+list navigation instead of touch.
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 phase 6)
//
// Named eapol_sniff, not eapol_capture, to avoid colliding with that
// file's existing CYD filenames (see subghz_sentinel.h for the incident
// that made this rule explicit).
//
// Two-phase lifecycle, same shape as the original's Phase enum:
//   SCAN    — a WiFi scan builds a target AP list (mirrors wifi_scan.cpp).
//   CAPTURE — promiscuous capture targeted at the selected AP's BSSID/
//             channel, with deauth injection to force a handshake. Same
//             extraction math as the original (PMKID from msg1's RSN IE,
//             MIC from msg2, ANonce, STA MAC), auto-saved to PCAP +
//             .hc22000 the moment a PMKID or full handshake completes,
//             and again on exit as a backstop — matching the original's
//             belt-and-suspenders save logic.
//
// Input mapping (no touchscreen here): rotate = move selection in the AP
// list; press = select an AP (SCAN phase) or toggle deauth on/off
// (CAPTURE phase, replacing the original's on-screen DEAUTH button); long
// press = save now (CAPTURE phase, replacing the original's on-screen SAVE
// button); BACK stops capture and returns to a fresh scan (CAPTURE phase)
// or exits the feature entirely (SCAN/list phase) — same transitions as the
// original.
// ═══════════════════════════════════════════════════════════════════════════

// Starts the AP scan worker on core 0 (SCAN phase).
void eapolSniffScanStart();

// Cancel an in-progress scan / abandon the AP list. Blocks until any
// scan worker has exited.
void eapolSniffScanStop();

const char *const *eapolSniffGetApResultLines();
int eapolSniffGetApResultCount();

// True if apList[index] is WPA2+ and can therefore produce EAPOL. Mirrors
// eapol_capture.cpp's checkAPListTouch() guard, which refuses to select
// open/WEP rows. The orchestrator must not transition to CAPTURE unless
// this returns true.
bool eapolSniffApHasEapol(int index);

// Selects apList[index] as the target and starts the capture worker
// (CAPTURE phase). Call only while the AP list from the last scan is
// still valid.
void eapolSniffSelectTarget(int index);

// Toggles continuous deauth injection against the current target on/off
// (CAPTURE phase only — the encoder-press equivalent of the original's
// on-screen DEAUTH button).
void eapolSniffToggleDeauth();

// Force-saves the current capture (CAPTURE phase only — the encoder
// long-press equivalent of the original's on-screen SAVE button) and
// re-arms the saved flags so later captures save again too.
void eapolSniffSaveNow();

// Stops the capture worker (auto-saves PCAP/.hc22000 if anything was
// captured and not yet saved). Blocks until the worker has exited.
void eapolSniffCaptureStop();

#endif // EAPOL_SNIFF_H
