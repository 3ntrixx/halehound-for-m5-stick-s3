#ifndef STATION_SCAN_H
#define STATION_SCAN_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Station Scanner — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A full-fidelity port of wifi_attacks.cpp's StationScan namespace's
// passive discovery core: promiscuous-mode capture of BOTH Probe Requests
// (a client searching, no AP association known) and Data frames (a client
// actively talking to an AP — ToDS/FromDS bit parsing recovers the AP's
// BSSID and channel, same offsets as the original), the same OUI vendor
// lookup table plus an optional IEEE MA-L registry on SD (/oui.bin), and the same 300ms/13-channel
// hop. Live-updating list of discovered stations (deduped by MAC, RSSI/
// vendor/AP updated in place) — same two-state RUNNING->RESULTS-with-live-
// refresh shape as ble_scan.cpp/probe_sniffer.cpp.
//
// NOT ported: the multi-select checkbox UI and its deauth-selected-clients
// handoff. That handoff needs per-station (unicast) deauth targeting,
// which deauther.h already documents as unported here — StationScan's own
// value as a recon tool (which client talks to which AP, from where)
// stands on its own; wiring it into an attack is future work once
// per-station deauth exists. Also not ported: the age-based row-color
// staleness indicator (visual only) and touch-driven paging.
// ═══════════════════════════════════════════════════════════════════════════

void stationScanStart();
void stationScanStop();

const char *const *stationScanGetResultLines();
int stationScanGetResultCount();

#endif // STATION_SCAN_H
