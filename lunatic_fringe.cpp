// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Lunatic Fringe — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// See lunatic_fringe.h for the full fidelity-decision writeup. Structurally
// this is ble_scan.cpp's worker task (radio_worker acquires RADIO_BLE, a
// BLEAdvertisedDeviceCallbacks stages one advertisement at a time into a
// volatile mailbox, the task drains it and live-updates a dedup-by-MAC
// device table) with bluetooth_attacks.cpp's LunaticFringe::
// processScanResults() classification logic dropped in verbatim in place
// of ble_database.h's company-ID lookup.
// ═══════════════════════════════════════════════════════════════════════════

#include "lunatic_fringe.h"
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
#define RESULT_LINE_LEN 40
#define LF_SCAN_WINDOW_SEC 5      // one scan window; auto-chained for continuous listening
#define LF_REFRESH_MIN_MS  1000   // matches ble_scan.cpp's live-refresh throttle

enum TrackerType : uint8_t {
    TRACKER_GOOGLE_FMDN = 0,
    TRACKER_SAMSUNG_TAG,
    TRACKER_TILE,
    TRACKER_CHIPOLO,
    TRACKER_APPLE_AIRTAG,
    TRACKER_APPLE_FINDMY,
    TRACKER_UNKNOWN
};

static const char *typeShort(TrackerType t) {
    switch (t) {
        case TRACKER_GOOGLE_FMDN:  return "FMDN";
        case TRACKER_SAMSUNG_TAG:  return "SMRT";
        case TRACKER_TILE:         return "TILE";
        case TRACKER_CHIPOLO:      return "CHIP";
        case TRACKER_APPLE_AIRTAG: return "ATAG";
        case TRACKER_APPLE_FINDMY: return "FMY ";
        default:                   return "????";
    }
}

// Ported verbatim from LunaticFringe::tdProximityStr().
static const char *proximityStr(int rssi) {
    if (rssi > -40) return "<1m";
    if (rssi > -55) return "1-3m";
    if (rssi > -70) return "3-8m";
    if (rssi > -85) return "8-15m";
    return ">15m";
}

static char s_resultBuf[MAX_RESULTS][RESULT_LINE_LEN];
static const char *s_resultPtrs[MAX_RESULTS];
static int s_resultCount = 0;

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Live tracker table (dedup by MAC, insertion order) ───────────────────
struct TrackedDevice {
    uint8_t mac[6];
    int rssi;
    TrackerType type;
};
static TrackedDevice s_devices[MAX_RESULTS];
static int s_deviceCount = 0;

static bool s_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) {
        s_classicBtReleased = true;
    }
    #if CYD_DEBUG
    Serial.printf("[LUNATIC] releaseClassicBtMemory: 0x%x, heap=%u\n", err, ESP.getFreeHeap());
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

class LunaticFringeCallbacks : public BLEAdvertisedDeviceCallbacks {
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
static LunaticFringeCallbacks s_callbacks;

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

// Ported from LunaticFringe::addOrUpdateDevice() — returns true if the
// table actually changed (new device, or an existing one's RSSI moved).
static bool addOrUpdateDevice(TrackerType type) {
    int idx = findDeviceIndex(s_pendingMac);
    if (idx >= 0) {
        bool changed = (s_devices[idx].rssi != s_pendingRssi);
        s_devices[idx].rssi = s_pendingRssi;
        return changed;
    }
    if (s_deviceCount >= MAX_RESULTS) return false; // table full — drop new MACs, keep existing live
    idx = s_deviceCount++;
    memcpy(s_devices[idx].mac, s_pendingMac, 6);
    s_devices[idx].rssi = s_pendingRssi;
    s_devices[idx].type = type;
    return true;
}

// Ported verbatim from LunaticFringe::processScanResults() — same UUIDs,
// frame-type/byte checks and priority order (service data first, then
// Apple manufacturer data) as the original.
static bool classifyAndTrack() {
    if (s_pendingHasServiceData) {
        uint16_t uuid = s_pendingServiceDataUUID16;
        const uint8_t *sd = s_pendingServiceData;
        uint8_t sdLen = s_pendingServiceDataLen;

        if (uuid == 0xFEAA && sdLen >= 1) { // Google FMDN
            uint8_t frameType = sd[0];
            if (frameType == 0x40 || frameType == 0x41) {
                return addOrUpdateDevice(TRACKER_GOOGLE_FMDN);
            }
            return false;
        }
        if ((uuid == 0xFD5A || uuid == 0xFD59) && sdLen >= 1) { // Samsung SmartTag (current + legacy UUID)
            if ((sd[0] & 0xF8) == 0x10) {
                return addOrUpdateDevice(TRACKER_SAMSUNG_TAG);
            }
            return false;
        }
        if (uuid == 0xFEED && sdLen >= 2) { // Tile
            if (sd[0] == 0x02 && sd[1] == 0x00) {
                return addOrUpdateDevice(TRACKER_TILE);
            }
            return false;
        }
        if (uuid == 0xFD84) { // Tile (newer service UUID)
            return addOrUpdateDevice(TRACKER_TILE);
        }
        if (uuid == 0xFE33) { // Chipolo — any service data present
            return addOrUpdateDevice(TRACKER_CHIPOLO);
        }
    }

    if (s_pendingHasMfgData && s_pendingMfgDataLen >= 4) { // Apple AirTag
        uint8_t compLow  = s_pendingMfgData[0];
        uint8_t compHigh = s_pendingMfgData[1];
        uint8_t type     = s_pendingMfgData[2];
        uint8_t len       = s_pendingMfgData[3];
        if (compLow == 0x4C && compHigh == 0x00 && type == 0x12 && len == 0x19) {
            return addOrUpdateDevice(TRACKER_APPLE_AIRTAG);
        }
        // Other Find My network accessories (non-AirTag offline-finding frames)
        if (compLow == 0x4C && compHigh == 0x00 && type == 0x12) {
            return addOrUpdateDevice(TRACKER_APPLE_FINDMY);
        }
    }

    return false;
}

static void formatResults() {
    s_resultCount = (s_deviceCount > MAX_RESULTS) ? MAX_RESULTS : s_deviceCount;
    for (int i = 0; i < s_resultCount; i++) {
        const TrackedDevice &d = s_devices[i];
        snprintf(s_resultBuf[i], RESULT_LINE_LEN, "%s %02X:%02X:%02X %ddBm %s",
                 typeShort(d.type), d.mac[3], d.mac[4], d.mac[5], d.rssi, proximityStr(d.rssi));
        s_resultPtrs[i] = s_resultBuf[i];
    }
    if (s_resultCount == 0) {
        snprintf(s_resultBuf[0], RESULT_LINE_LEN, "(no trackers found)");
        s_resultPtrs[0] = s_resultBuf[0];
        s_resultCount = 1;
    }
}

static void postRefresh() {
    formatResults();
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE;
    messagingPostTelemetry(msg);
}

static void lunaticFringeWorkerTask(void *param) {
    (void)param;

    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    BLEScan *pBleScan = BLEDevice::getScan();
    if (!pBleScan) {
        #if CYD_DEBUG
        Serial.println("[LUNATIC] ERROR: getScan() returned NULL");
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
            if (!transitioned) {
                transitioned = true;
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

void lunaticFringeStart() {
    if (!radioAcquire(RADIO_BLE)) {
        #if CYD_DEBUG
        Serial.println("[LUNATIC] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_resultCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(lunaticFringeWorkerTask, "lunatic_fringe", 6144, NULL, 2, &s_workerHandle, 0);
}

void lunaticFringeStop() {
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

const char *const *lunaticFringeGetResultLines() {
    return s_resultPtrs;
}

int lunaticFringeGetResultCount() {
    return s_resultCount;
}
