// ═══════════════════════════════════════════════════════════════════════════
// HaleHound AirTag Tracker — implementation
// Passive BLE scan that reports Apple Find My network advertisements
// (anti-stalking aid: shows nearby AirTags and other Find My accessories
// with a live signal-strength proximity estimate).
// Same worker shape as lunatic_fringe.cpp (Tracker Scan), filtered to Apple.
// ═══════════════════════════════════════════════════════════════════════════

#include "airtag_tracker.h"
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_bt.h>
#include <esp_bt_defs.h> // esp_bt_uuid_t / ESP_UUID_LEN_16
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define MAX_RESULTS 20
#define LF_SCAN_WINDOW_SEC 5      // one scan window; auto-chained for continuous listening
#define LF_REFRESH_MIN_MS  1000   // matches ble_scan.cpp's live-refresh throttle
#define LF_ACTIVE_WINDOW_MS 10000 // "active" dot: seen within the last 10s

enum TrackerType : uint8_t {
    TRACKER_GOOGLE_FMDN = 0,
    TRACKER_SAMSUNG_TAG,
    TRACKER_TILE,
    TRACKER_CHIPOLO,
    TRACKER_APPLE_AIRTAG,
    TRACKER_APPLE_FINDMY,
    TRACKER_UNKNOWN
};

// Apple's offline-finding status byte (manufacturer-data index 4) packs
// battery state into its top 2 bits — reverse-engineered mapping used by
// third-party Find My detectors (e.g. the AirGuard project), not published
// by Apple; shown here as a best-effort label, matching the reference
// HaleHound build's AirTag Detect screen.
enum BatteryLevel : uint8_t {
    BATTERY_FULL = 0,
    BATTERY_MEDIUM,
    BATTERY_LOW,
    BATTERY_CRITICAL,
    BATTERY_UNKNOWN
};

static const char *batteryStr(BatteryLevel b) {
    switch (b) {
        case BATTERY_FULL:     return "FULL";
        case BATTERY_MEDIUM:   return "MED ";
        case BATTERY_LOW:      return "LOW ";
        case BATTERY_CRITICAL: return "CRIT";
        default:               return "?   ";
    }
}

// Ported verbatim from AirTagTracker::tdProximityStr().
static const char *proximityStr(int rssi) {
    if (rssi > -40) return "<1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return ">15m";
}

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Live tracker table (dedup by MAC, insertion order) ───────────────────
struct TrackedDevice {
    uint8_t mac[6];
    int rssi;
    TrackerType type;
    BatteryLevel battery; // only meaningful for TRACKER_APPLE_AIRTAG
    uint32_t lastSeenMs;
};
static TrackedDevice s_devices[MAX_RESULTS];
static int s_deviceCount = 0;
static int s_scanCount = 0; // completed 5s scan windows, for the SCANS: N header

static bool s_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) {
        s_classicBtReleased = true;
    }
    #if CYD_DEBUG
    Serial.printf("[AIRTAG] releaseClassicBtMemory: 0x%x, heap=%u\n", err, ESP.getFreeHeap());
    #endif
}

// ── Advertisement callback → single-slot pending mailbox, same shape as
// ble_scan.cpp's BleScanCallbacks — runs off the BLE stack's own task
// context, so it just stages the raw fields needed for classification.
static volatile bool s_pendingReady = false;
static uint8_t s_pendingMac[6];
static int s_pendingRssi;
static bool s_pendingHasServiceData;
static uint16_t s_pendingServiceDataUUID16; // only 16-bit UUIDs are used by any tracked fingerprint
static uint8_t s_pendingServiceData[4];
static uint8_t s_pendingServiceDataLen;
static bool s_pendingHasMfgData;
static uint8_t s_pendingMfgData[8];
static uint8_t s_pendingMfgDataLen;

class AirTagTrackerCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        if (s_pendingReady) return; // previous frame not yet drained — skip, matches ble_scan.cpp

        const uint8_t *addr = *advertisedDevice.getAddress().getNative();
        memcpy((void *)s_pendingMac, addr, 6);
        s_pendingRssi = advertisedDevice.getRSSI();

        esp_bt_uuid_t *native = advertisedDevice.haveServiceData()
            ? advertisedDevice.getServiceDataUUID().getNative() : nullptr;
        if (native && native->len == ESP_UUID_LEN_16) {
            // Every tracked fingerprint uses a 16-bit BT SIG assigned
            // service UUID — a 32-/128-bit service data UUID can never
            // match one, so skip it rather than misreading raw UUID128
            // bytes through the uuid16 union member.
            std::string sd = advertisedDevice.getServiceData();
            s_pendingHasServiceData = true;
            s_pendingServiceDataUUID16 = native->uuid.uuid16;
            s_pendingServiceDataLen = (sd.length() > 4) ? 4 : (uint8_t)sd.length();
            memcpy((void *)s_pendingServiceData, sd.data(), s_pendingServiceDataLen);
        } else {
            s_pendingHasServiceData = false;
        }

        if (advertisedDevice.haveManufacturerData()) {
            std::string mfg = advertisedDevice.getManufacturerData();
            s_pendingHasMfgData = true;
            s_pendingMfgDataLen = (mfg.length() > 8) ? 8 : (uint8_t)mfg.length();
            memcpy((void *)s_pendingMfgData, mfg.data(), s_pendingMfgDataLen);
        } else {
            s_pendingHasMfgData = false;
        }

        s_pendingReady = true;
    }
};
static AirTagTrackerCallbacks s_callbacks;

static volatile bool s_scanWindowEnded = false;
static void onScanWindowEnded(BLEScanResults results) {
    (void)results;
    s_scanWindowEnded = true; // task loop restarts the next window
}

static int findDeviceIndex(const uint8_t *mac) {
    for (int i = 0; i < s_deviceCount; i++) {
        if (memcmp(s_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// Ported from AirTagTracker::addOrUpdateDevice() — returns true if the
// table actually changed (new device, an existing one's RSSI moved, or its
// decoded battery state changed).
static bool addOrUpdateDevice(TrackerType type, BatteryLevel battery) {
    int idx = findDeviceIndex(s_pendingMac);
    if (idx >= 0) {
        bool changed = (s_devices[idx].rssi != s_pendingRssi) || (s_devices[idx].battery != battery);
        s_devices[idx].rssi = s_pendingRssi;
        s_devices[idx].battery = battery;
        s_devices[idx].lastSeenMs = millis();
        return changed;
    }
    if (s_deviceCount >= MAX_RESULTS) return false; // table full — drop new MACs, keep existing live
    idx = s_deviceCount++;
    memcpy(s_devices[idx].mac, s_pendingMac, 6);
    s_devices[idx].rssi = s_pendingRssi;
    s_devices[idx].type = type;
    s_devices[idx].battery = battery;
    s_devices[idx].lastSeenMs = millis();
    return true;
}

// Apple Find My network classifier — manufacturer data (company 0x004C),
// offline-finding frame type 0x12. Payload length 0x19 is a genuine AirTag;
// any other length is some other Find My network accessory (AirPods case,
// Apple Watch, a lost iPhone, third-party Find My tags, etc.) — still worth
// surfacing for this anti-stalking scan, same two-tier check as
// lunatic_fringe.cpp's classifyAndTrack().
static bool classifyAndTrack() {
    if (s_pendingHasMfgData && s_pendingMfgDataLen >= 4) {
        uint8_t compLow  = s_pendingMfgData[0];
        uint8_t compHigh = s_pendingMfgData[1];
        uint8_t type     = s_pendingMfgData[2];
        uint8_t len      = s_pendingMfgData[3];
        if (compLow == 0x4C && compHigh == 0x00 && type == 0x12 && len == 0x19) {
            BatteryLevel battery = BATTERY_UNKNOWN;
            if (s_pendingMfgDataLen >= 5) {
                battery = (BatteryLevel)((s_pendingMfgData[4] >> 6) & 0x03);
            }
            return addOrUpdateDevice(TRACKER_APPLE_AIRTAG, battery);
        }
        if (compLow == 0x4C && compHigh == 0x00 && type == 0x12) {
            // Battery bits are only documented for AirTag's own status byte;
            // other Find My accessories may not use the same encoding.
            return addOrUpdateDevice(TRACKER_APPLE_FINDMY, BATTERY_UNKNOWN);
        }
    }
    return false;
}

static void postRefresh() {
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE;
    messagingPostTelemetry(msg);
}

static void airtagTrackerWorkerTask(void *param) {
    (void)param;

    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    BLEScan *pBleScan = BLEDevice::getScan();
    if (!pBleScan) {
        #if CYD_DEBUG
        Serial.println("[AIRTAG] ERROR: getScan() returned NULL");
        #endif
        BLEDevice::deinit(false);
        WiFi.mode(WIFI_STA);
        radioRelease(RADIO_BLE);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    pBleScan->setActiveScan(true); // active scan for full advertisement/service data, matches the original
    pBleScan->setInterval(100);
    pBleScan->setWindow(99);
    pBleScan->setAdvertisedDeviceCallbacks(&s_callbacks, true); // wantDuplicates=true — repeat sightings update RSSI live

    s_deviceCount = 0;
    s_scanWindowEnded = false;
    pBleScan->start(LF_SCAN_WINDOW_SEC, onScanWindowEnded, false);

    bool transitioned = false;
    bool dirty = false;
    uint32_t lastRefresh = 0;

    for (;;) {
        if (s_pendingReady) {
            if (classifyAndTrack()) dirty = true;
            s_pendingReady = false;
        }

        if (s_scanWindowEnded) {
            s_scanWindowEnded = false;
            s_scanCount++;
            transitioned = true;
            // Always refresh while nothing's been found yet (so the idle
            // screen's SCANS: N counter keeps advancing every window), same
            // as any other dirty-triggered refresh once devices exist.
            if (dirty || s_deviceCount == 0) {
                dirty = false;
                lastRefresh = millis();
                postRefresh();
            }
            pBleScan->start(LF_SCAN_WINDOW_SEC, onScanWindowEnded, false);
        }

        uint32_t now = millis();
        if (transitioned && dirty && (now - lastRefresh >= LF_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            postRefresh();
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            pBleScan->stop();
            break;
        }
    }

    pBleScan->setAdvertisedDeviceCallbacks(nullptr, false);
    BLEDevice::deinit(false);
    WiFi.mode(WIFI_STA);
    radioRelease(RADIO_BLE);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void airtagTrackerStart() {
    if (!radioAcquire(RADIO_BLE)) {
        #if CYD_DEBUG
        Serial.println("[AIRTAG] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_scanCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(airtagTrackerWorkerTask, "airtag_tracker", 6144, NULL, 2, &s_workerHandle, 0);
}

void airtagTrackerStop() {
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

int airtagTrackerGetDeviceCount() {
    return s_deviceCount;
}

int airtagTrackerGetScanCount() {
    return s_scanCount;
}

int airtagTrackerGetDisplayRows(TrackerDisplayRow *out, int max) {
    int n = s_deviceCount;
    if (n > max) n = max;
    if (n < 0) n = 0;
    uint32_t now = millis();
    for (int i = 0; i < n; i++) {
        const TrackedDevice &d = s_devices[i];
        TrackerDisplayRow &r = out[i];
        snprintf(r.col1, sizeof(r.col1), "%s", batteryStr(d.battery));
        snprintf(r.mac, sizeof(r.mac), "%02X:%02X:%02X", d.mac[3], d.mac[4], d.mac[5]);
        r.rssi = d.rssi;
        snprintf(r.proximity, sizeof(r.proximity), "%s", proximityStr(d.rssi));
        r.active = (now - d.lastSeenMs) < LF_ACTIVE_WINDOW_MS;
    }
    return n;
}
