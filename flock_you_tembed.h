#ifndef FLOCK_YOU_TEMBED_H
#define FLOCK_YOU_TEMBED_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Flock You — T-Embed CC1101 port
// Created: 2026-10-03
//
// A simplified port of bluetooth_attacks.cpp's FlockYou namespace: passive
// BLE scan with OUI / name / manufacturer-ID / service-UUID classifier
// against the colonelpanichacks/flock-you tables, flagging Flock Safety
// ALPR cameras, Raven gunshot detectors, SoundThinking acoustic sensors
// and associated infrastructure.
//
// The CYD build adds a touchscreen detail overlay with per-type animated
// alert bars, Raven firmware-version estimation, and SD-logged intel.
// This port keeps the detection core and drops those: it's a one-screen
// scrolling list — "<type> <name|mac3> <rssi>dBm" — refreshed live as
// matches come in. Detail overlay and SD intel log can be grafted on later
// without reshaping the orchestrator wiring (same shape as ble_scan.cpp).
//
// Structurally follows ble_scan.cpp's BlePredator LISTEN port: Bluedroid
// init, passive scan with 5-second windows auto-chained via the completion
// callback, device table deduped by MAC and updated in place, display
// snapshot formatted once per BLE_REFRESH_MIN_MS when dirty.
// ═══════════════════════════════════════════════════════════════════════════

void flockYouStart();
void flockYouStop();

// Result accessors — populated by the worker, read by the orchestrator.
const char *const *flockYouGetResultLines();
int                flockYouGetResultCount();

// Header counters — "Found: 3 | Cams: 1 | Ravens: 1" style.
int flockYouGetDeviceCount();   // total matched devices currently alive
int flockYouGetCamCount();      // EXT_BATT + WIFI_MOD + DIRECT_REG + MFR_MATCH
int flockYouGetRavenCount();    // RAVEN + PENGUIN + SOUNDTHINK

#endif // FLOCK_YOU_TEMBED_H
