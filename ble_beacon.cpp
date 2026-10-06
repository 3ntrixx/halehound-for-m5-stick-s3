// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Beacon — implementation
// Created: 2026-09-21 (Cardputer ADV port)
// Shared unchanged with the T-Embed CC1101 build since 2026-09-22.
// See ble_beacon.h for the design writeup, mode list and what was left out.
// ═══════════════════════════════════════════════════════════════════════════

#include "ble_beacon.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>
#include <esp_bt.h>
#include <esp_random.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define BLE_BEACON_STATUS_INTERVAL_MS 250

// Mode order matches bluetooth_attacks.cpp's BleBeacon::BeaconMode exactly —
// see ble_beacon.h for what each mode broadcasts.
enum BeaconMode {
    BCN_RANDOM_IBEACON = 0,
    BCN_APPLE_STORE,
    BCN_STARBUCKS,
    BCN_EDDYSTONE_URL,
    BCN_EDDYSTONE_UID,
    BCN_GEO_FENCE,
    BCN_MODE_COUNT
};

static const char *const BCN_MODE_NAMES[BCN_MODE_COUNT] = {
    "RANDOM iBEACON",
    "APPLE STORE",
    "STARBUCKS",
    "EDDYSTONE URL",
    "EDDYSTONE UID",
    "GEO-FENCE",
};

// ─────────────────────────────────────────────────────────────────────────
// iBEACON PRESETS — identical UUID bytes to bluetooth_attacks.cpp's
// BleBeacon namespace.
// ─────────────────────────────────────────────────────────────────────────

// Apple Store iBeacon UUID: E2C56DB5-DFFB-48D2-B060-D0F5A71096E0
static const uint8_t UUID_APPLE_STORE[16] = {
    0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2,
    0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0
};

// Starbucks retail beacon UUID: 2F234454-CF6D-4A0F-ADF2-F4911BA9FFA6
static const uint8_t UUID_STARBUCKS[16] = {
    0x2F, 0x23, 0x44, 0x54, 0xCF, 0x6D, 0x4A, 0x0F,
    0xAD, 0xF2, 0xF4, 0x91, 0x1B, 0xA9, 0xFF, 0xA6
};

// Target retail beacon UUID: AA6062F0-98CA-4211-8EC4-3D0B20EA9616
static const uint8_t UUID_TARGET[16] = {
    0xAA, 0x60, 0x62, 0xF0, 0x98, 0xCA, 0x42, 0x11,
    0x8E, 0xC4, 0x3D, 0x0B, 0x20, 0xEA, 0x96, 0x16
};

// Walmart retail beacon UUID: 74278BDA-B644-4520-8F0C-720EAF059935
static const uint8_t UUID_WALMART[16] = {
    0x74, 0x27, 0x8B, 0xDA, 0xB6, 0x44, 0x45, 0x20,
    0x8F, 0x0C, 0x72, 0x0E, 0xAF, 0x05, 0x99, 0x35
};

// Macy's retail beacon UUID: B9407F30-F5F8-466E-AFF9-25556B57FE6D (Estimote)
static const uint8_t UUID_MACYS[16] = {
    0xB9, 0x40, 0x7F, 0x30, 0xF5, 0xF8, 0x46, 0x6E,
    0xAF, 0xF9, 0x25, 0x55, 0x6B, 0x57, 0xFE, 0x6D
};

// Geo-fence preset table — cycled one-per-packet by BCN_GEO_FENCE.
#define GEO_FENCE_PRESET_COUNT 5
static const uint8_t *const GEO_FENCE_UUIDS[GEO_FENCE_PRESET_COUNT] = {
    UUID_APPLE_STORE, UUID_STARBUCKS, UUID_TARGET, UUID_WALMART, UUID_MACYS
};
static const char *const GEO_FENCE_NAMES[GEO_FENCE_PRESET_COUNT] = {
    "AppleStore", "Starbucks", "Target", "Walmart", "Macys"
};

static const char *const EDDYSTONE_URL_PRESET = "google.com";

// Raw ESP-IDF advertising params — identical to the original's bcnAdvParams:
// non-connectable undirected (beacon standard), random static MAC, all 3
// advertising channels, 0xA0 (100ms) interval.
static esp_ble_adv_params_t s_advParams = {
    .adv_int_min = 0xA0,
    .adv_int_max = 0xA0,
    .adv_type = ADV_TYPE_NONCONN_IND,
    .own_addr_type = BLE_ADDR_TYPE_RANDOM,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// Shared directly with the worker task, same idiom as ble_jammer.cpp's
// s_modeIndex / beacon_spammer.cpp's s_channel (no queue — mode changes are
// live, not commands the worker must acknowledge).
static volatile int s_modeIndex = BCN_RANDOM_IBEACON;
static volatile uint32_t s_packetCount = 0;

// GEO-FENCE preset cursor — worker-task-only (advanced and read from inside
// doBroadcast(), never touched from the caller's core), unlike s_modeIndex.
static int s_geoFenceStep = 0;

// Same duplicated-per-file pattern every other on-chip BLE module in this
// port uses (ble_scan.cpp, ble_skeletonkey.cpp, lunatic_fringe.cpp) — not
// extracted into a shared helper, since the port plan's "shared BLE
// runtime" phase was never built. Must run before BLEDevice::init() (which
// triggers esp_bt_controller_init()) — HaleHound only uses BLE, so Classic
// BT's ~28KB controller reservation is freed permanently on first use.
static bool s_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) {
        s_classicBtReleased = true;
    }
    #if CYD_DEBUG
    Serial.printf("[BLE_BEACON] releaseClassicBtMemory: 0x%x, heap=%u\n", err, ESP.getFreeHeap());
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// PACKET BUILDERS — byte-for-byte port of bluetooth_attacks.cpp's
// buildIBeaconPacket()/buildEddystoneURL()/buildEddystoneUID(). No TFT/
// touch/EEPROM dependency in the originals, so these carried over unchanged.
// ═══════════════════════════════════════════════════════════════════════════

// Standard 30-byte iBeacon advertisement payload: Flags(3) + Mfr(0xFF) +
// Apple(4C00) + iBeacon type/len(0215) + UUID(16) + Major(2) + Minor(2) +
// TX Power(1). Returns total packet length (always 30).
static int buildIBeaconPacket(uint8_t *buf, const uint8_t *uuid, uint16_t major, uint16_t minor, int8_t txPower) {
    int i = 0;
    // Flags AD structure
    buf[i++] = 0x02;  // Length
    buf[i++] = 0x01;  // Flags type
    buf[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    // Manufacturer Specific Data
    buf[i++] = 0x1A;  // AD Length = 26 bytes follow
    buf[i++] = 0xFF;  // Manufacturer Specific Data type
    buf[i++] = 0x4C;  // Apple Inc (low byte)
    buf[i++] = 0x00;  // Apple Inc (high byte)
    buf[i++] = 0x02;  // iBeacon type
    buf[i++] = 0x15;  // iBeacon data length (21 bytes)
    // UUID (16 bytes)
    memcpy(&buf[i], uuid, 16);
    i += 16;
    // Major (big-endian)
    buf[i++] = (major >> 8) & 0xFF;
    buf[i++] = major & 0xFF;
    // Minor (big-endian)
    buf[i++] = (minor >> 8) & 0xFF;
    buf[i++] = minor & 0xFF;
    // TX Power (calibrated at 1m)
    buf[i++] = (uint8_t)txPower;
    return i;  // 30
}

// Eddystone-URL advertisement payload: service UUID 0xFEAA, "https://www."
// scheme prefix, .com expansion code per the Eddystone spec (max 17 encoded
// URL bytes). Returns total packet length.
static int buildEddystoneURL(uint8_t *buf, const char *url) {
    int i = 0;
    // Flags AD structure
    buf[i++] = 0x02;  // Length
    buf[i++] = 0x01;  // Flags type
    buf[i++] = 0x06;  // General Discoverable + BR/EDR Not Supported
    // Complete 16-bit Service UUID (Eddystone 0xFEAA)
    buf[i++] = 0x03;  // Length
    buf[i++] = 0x03;  // Complete 16-bit UUID list type
    buf[i++] = 0xAA;  // 0xFEAA little-endian low
    buf[i++] = 0xFE;  // 0xFEAA little-endian high
    // Service Data start — length filled at end
    int svcLenPos = i;
    buf[i++] = 0x00;  // Placeholder for service data length
    buf[i++] = 0x16;  // Service Data type
    buf[i++] = 0xAA;  // 0xFEAA little-endian low
    buf[i++] = 0xFE;  // 0xFEAA little-endian high
    // Eddystone-URL frame
    buf[i++] = 0x10;  // Frame type: URL
    buf[i++] = 0xF4;  // TX Power at 0m (-12 dBm typical)
    // URL scheme prefix
    buf[i++] = 0x01;  // "https://www."
    // Encode URL (with .com expansion code per Eddystone spec)
    int urlLen = strlen(url);
    if (urlLen > 17) urlLen = 17;  // Max URL bytes in BLE adv
    for (int u = 0; u < urlLen; u++) {
        // Encode .com as 0x00 per Eddystone spec
        if (u + 3 < urlLen && url[u] == '.' && url[u + 1] == 'c' && url[u + 2] == 'o' && url[u + 3] == 'm') {
            buf[i++] = 0x00;  // .com expansion code
            u += 3;  // Skip past ".com"
        } else {
            buf[i++] = (uint8_t)url[u];
        }
    }
    // Fill in service data length (everything after the length byte)
    buf[svcLenPos] = (uint8_t)(i - svcLenPos - 1);
    return i;
}

// Eddystone-UID advertisement payload: 10-byte namespace + 6-byte instance,
// service UUID 0xFEAA. Returns total packet length (always 25).
static int buildEddystoneUID(uint8_t *buf, const uint8_t *ns, const uint8_t *instance) {
    int i = 0;
    buf[i++] = 0x02;
    buf[i++] = 0x01;
    buf[i++] = 0x06;
    // Complete 16-bit Service UUID (Eddystone 0xFEAA)
    buf[i++] = 0x03;
    buf[i++] = 0x03;
    buf[i++] = 0xAA;
    buf[i++] = 0xFE;
    // Service Data
    buf[i++] = 0x15;  // Length: 21 bytes follow
    buf[i++] = 0x16;  // Service Data type
    buf[i++] = 0xAA;
    buf[i++] = 0xFE;
    // Eddystone-UID frame
    buf[i++] = 0x00;  // Frame type: UID
    buf[i++] = 0xF4;  // TX Power at 0m
    // Namespace (10 bytes)
    memcpy(&buf[i], ns, 10);
    i += 10;
    // Instance (6 bytes)
    memcpy(&buf[i], instance, 6);
    i += 6;
    // Reserved (2 bytes, must be 0x00)
    buf[i++] = 0x00;
    buf[i++] = 0x00;
    return i;  // 25
}

// ═══════════════════════════════════════════════════════════════════════════
// BROADCAST ENGINE — port of doBroadcast(): fresh random MAC + payload each
// cycle via the same raw esp_ble_gap_*() calls, ~21ms/cycle (delay(1) after
// loading the payload, delay(20) advertising, then stop). `detail` is
// filled with the per-packet distinguishing text (UUID major:minor, URL, or
// namespace prefix) that the original wrote into its scrolling log line —
// the caller folds it into the single status line instead (see ble_beacon.h
// on what replaced the log).
// ═══════════════════════════════════════════════════════════════════════════

static void doBroadcast(char *detail, size_t detailLen) {
    // Stop any current advertising
    esp_ble_gap_stop_advertising();

    // Generate random BLE MAC address
    esp_bd_addr_t addr;
    esp_fill_random(addr, 6);
    addr[0] |= 0xC0;  // BLE random static address type
    esp_ble_gap_set_rand_addr(addr);

    // Build packet based on current mode
    uint8_t pkt[31];
    int pktLen = 0;

    switch (s_modeIndex) {
        case BCN_RANDOM_IBEACON: {
            // Random UUID, Major, Minor each cycle
            uint8_t randUUID[16];
            esp_fill_random(randUUID, 16);
            uint16_t major = (uint16_t)random(0xFFFF);
            uint16_t minor = (uint16_t)random(0xFFFF);
            pktLen = buildIBeaconPacket(pkt, randUUID, major, minor, -59);
            snprintf(detail, detailLen, "%04X:%04X", major, minor);
            break;
        }
        case BCN_APPLE_STORE: {
            uint16_t major = 1 + (uint16_t)random(50);   // Store section
            uint16_t minor = 1 + (uint16_t)random(200);  // Fixture
            pktLen = buildIBeaconPacket(pkt, UUID_APPLE_STORE, major, minor, -59);
            snprintf(detail, detailLen, "%d:%d", major, minor);
            break;
        }
        case BCN_STARBUCKS: {
            uint16_t major = 100 + (uint16_t)random(900);
            uint16_t minor = 1 + (uint16_t)random(50);
            pktLen = buildIBeaconPacket(pkt, UUID_STARBUCKS, major, minor, -59);
            snprintf(detail, detailLen, "%d:%d", major, minor);
            break;
        }
        case BCN_EDDYSTONE_URL: {
            pktLen = buildEddystoneURL(pkt, EDDYSTONE_URL_PRESET);
            snprintf(detail, detailLen, "%s", EDDYSTONE_URL_PRESET);
            break;
        }
        case BCN_EDDYSTONE_UID: {
            uint8_t ns[10];
            uint8_t instance[6];
            esp_fill_random(ns, 10);
            esp_fill_random(instance, 6);
            pktLen = buildEddystoneUID(pkt, ns, instance);
            snprintf(detail, detailLen, "%02X%02X:%02X%02X", ns[0], ns[1], instance[0], instance[1]);
            break;
        }
        case BCN_GEO_FENCE: {
            // Cycle through all retail presets rapidly, one per packet
            int presetIdx = s_geoFenceStep % GEO_FENCE_PRESET_COUNT;
            uint16_t major = 1 + (uint16_t)random(500);
            uint16_t minor = 1 + (uint16_t)random(500);
            pktLen = buildIBeaconPacket(pkt, GEO_FENCE_UUIDS[presetIdx], major, minor, -59);
            snprintf(detail, detailLen, "%s %d:%d", GEO_FENCE_NAMES[presetIdx], major, minor);
            s_geoFenceStep++;
            break;
        }
        default:
            return;
    }

    // Set raw advertising data via ESP-IDF
    esp_ble_gap_config_adv_data_raw(pkt, pktLen);
    delay(1);

    // Start advertising
    esp_ble_gap_start_advertising(&s_advParams);
    delay(20);
    esp_ble_gap_stop_advertising();

    s_packetCount++;
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER TASK — Core 0. One task does BLE init/teardown, the broadcast loop
// and status telemetry, same single-task shape as ble_jammer.cpp's worker
// (BLE beacon's ~21ms/cycle pacing is already slow enough that it doesn't
// need beacon_spammer.cpp's separate outer-status/inner-TX task split).
// ═══════════════════════════════════════════════════════════════════════════

static void workerTask(void *param) {
    (void)param;

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);  // BLE controller needs time to finish internal init (matches original's BleBeacon::setup())

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    s_packetCount = 0;
    s_geoFenceStep = 0;

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

        char detail[24];
        detail[0] = '\0';
        doBroadcast(detail, sizeof(detail));

        uint32_t now = millis();
        if (now - lastStatus >= BLE_BEACON_STATUS_INTERVAL_MS) {
            lastStatus = now;
            RenderMsg msg;
            msg.kind = RENDER_MSG_STATUS_TEXT;
            snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "%s %s pkts:%lu",
                     BCN_MODE_NAMES[s_modeIndex], detail, (unsigned long)s_packetCount);
            messagingPostTelemetry(msg);
        }

        // doBroadcast()'s own delay(1)+delay(20) already yields; belt and
        // suspenders, matches the original's bcnTxTask.
        vTaskDelay(1);
    }

    esp_ble_gap_stop_advertising();
    BLEDevice::deinit(false);  // false = library bug: deinit(true) never resets initialized flag (matches every other BLE module's comment on this)

    radioRelease(RADIO_BLE);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void bleBeaconStart() {
    if (!radioAcquire(RADIO_BLE)) {
        #if CYD_DEBUG
        Serial.println("[BLE_BEACON] ERROR: could not acquire radio");
        #endif
        return;
    }

    // Original's setup() always reset currentMode to BCN_RANDOM_IBEACON on
    // screen entry (never persisted across sessions) — this port makes the
    // same choice on every bleBeaconStart(), rather than remembering the
    // last mode across a Stop/Start cycle.
    s_modeIndex = BCN_RANDOM_IBEACON;

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(workerTask, "ble_beacon", 8192, NULL, 1, &s_workerHandle, 0);
}

void bleBeaconStop() {
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

void bleBeaconAdjustMode(int delta) {
    int idx = s_modeIndex;
    idx = (idx + delta + BCN_MODE_COUNT) % BCN_MODE_COUNT;
    s_modeIndex = idx;
}
