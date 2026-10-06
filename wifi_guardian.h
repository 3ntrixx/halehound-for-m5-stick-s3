#ifndef WIFI_GUARDIAN_H
#define WIFI_GUARDIAN_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Guardian — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A full-fidelity port of jam_detect.cpp's WiFiGuardian namespace: promiscuous
// MGMT-frame counting (deauth/disassoc/beacon, via the same frame-control
// subtype bytes packet_monitor.cpp already checks), hopping 1/6/11 every
// 500ms, a 5s running-average baseline calibration, then per-second rate
// classification into the same CALIBRATING -> CLEAR -> SUSPICIOUS -> JAMMING
// threat level (and 3s hysteresis) already ported for subghz_sentinel.cpp —
// same ThreatLevel shape, different sensor (WiFi MGMT rates vs. CC1101 RSSI).
// Duplicated rather than shared, per this project's own stated precedent
// (subghz_sentinel.h: "extract a shared helper once a third module needs it").
//
// Not ported: the event log (5-entry scrolling text history) and the
// touch/skull-meter visuals — this project's uiRenderShowStatus() is a
// single status line, like every other *_RUNNING screen here; the threat
// level and per-second rates are all in that line instead.
// ═══════════════════════════════════════════════════════════════════════════

void wifiGuardianStart();
void wifiGuardianStop();

#endif // WIFI_GUARDIAN_H
