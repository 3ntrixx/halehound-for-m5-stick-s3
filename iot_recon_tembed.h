#ifndef IOT_RECON_TEMBED_H
#define IOT_RECON_TEMBED_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound IoT Recon — T-Embed CC1101 port
// Created: 2026-10-03
//
// A simplified port of iot_recon.cpp.  The CYD build is a full attack
// suite: WiFi connect → TCP port scan (RTSP/HTTP/Telnet/MQTT/Modbus) →
// banner grab → credential brute force against a 100+ entry camera/IoT
// default-cred dictionary, with on-screen keyboard for the WiFi password
// and SD-logged intel.
//
// None of that network attack surface fits a one-screen passive device,
// and the UX scaffolding it needs (SSID keyboard, saved-creds browser,
// kill feed with per-device detail overlay) is deferred elsewhere in the
// port plan.  This port keeps the "what IoT devices are in range" half:
// a periodic WiFi.scanNetworks() with SSID-pattern + BSSID-OUI
// classification against the CYD build's own device fingerprint tables
// (same vendor set it brute-forces).  The result is the recon output of
// the full suite — "there is a HIKVISION camera on channel 6" — without
// the connect-and-attack phase.
//
// Attack phase (connect / port scan / brute force) can be grafted on later
// without reshaping the orchestrator wiring (same NAV + loop telemetry
// shape as wifi_scan.cpp).
// ═══════════════════════════════════════════════════════════════════════════

void iotReconStart();
void iotReconStop();

// Result accessors — populated by the worker, read by the orchestrator.
const char *const *iotReconGetResultLines();
int                iotReconGetResultCount();

// Header counters for the submenu title line.
int iotReconGetDeviceCount();     // total IoT matches currently in the list
int iotReconGetCamCount();        // devices classified as cameras
int iotReconGetSmartHubCount();   // smart-home hubs (Nest/Ring/Alexa/etc)

#endif // IOT_RECON_TEMBED_H
