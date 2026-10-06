#ifndef PROBE_SNIFFER_H
#define PROBE_SNIFFER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Probe Sniffer — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A port of wifi_attacks.cpp's DeauthDetect namespace's core capture (the
// WiFi submenu's "Probe Sniffer" row, despite the internal name — see that
// file's own header comment): promiscuous probe-request (frame type 0x40)
// capture while hopping all 13 channels every 300ms, extracting the source
// MAC and the SSID tag (tag 0) from tagged parameters, deduping by full MAC
// to flag NEW vs. repeat devices, and a scrolling log of the most recent
// captures — same shapes/offsets/timings as the original. Deliberate
// efficiency deviations: an 8-slot SPSC ring instead of the original's
// single pending slot (the T-Embed worker can only drain so often, so the
// original silently dropped nearly every probe in a busy area), the worker
// blocks at most 5ms instead of 50ms between drains, a MGMT-only
// promiscuous filter, and 32-byte SSIDs are no longer skipped.
//
// Not ported: Preferences-based "Save Targets" persistence, the ALL/SSID/
// NEW filter-cycling UI, and the tap-a-probe-to-spawn-Evil-Twin handoff
// into CaptivePortal — this is a read-only live log, like every other
// continuously-updating list in this project (BLE Predator's device list,
// etc.).
// ═══════════════════════════════════════════════════════════════════════════

void probeSniffStart();
void probeSniffStop();

// Result accessors — like ble_scan.h, these stay valid (and keep updating)
// for as long as the feature is running, not just after one scan.
const char *const *probeSniffGetResultLines();
int probeSniffGetResultCount();

#endif // PROBE_SNIFFER_H
