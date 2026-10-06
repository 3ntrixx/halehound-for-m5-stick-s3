// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Flock You — T-Embed CC1101 implementation
// Created: 2026-10-03  (see flock_you_tembed.h for the port writeup)
// ═══════════════════════════════════════════════════════════════════════════

#include "flock_you_tembed.h"
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEUUID.h>
#include <esp_bt.h>
#include <ctype.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS  (match the CYD FlockYou namespace)
// ═══════════════════════════════════════════════════════════════════════════

#define FY_MAX_DEVICES       24
#define FY_RESULT_LINE_LEN   48
#define FY_SCAN_WINDOW_SEC    5
#define FY_REFRESH_MIN_MS   750
#define FY_STALE_TIMEOUT  30000

// ═══════════════════════════════════════════════════════════════════════════
// CLASSIFIER TABLES  (verbatim from FlockYou — colonelpanichacks/flock-you)
// ═══════════════════════════════════════════════════════════════════════════

enum FlockDevType : uint8_t {
    FLOCK_EXT_BATT    = 0,
    FLOCK_RAVEN       = 1,
    FLOCK_PENGUIN     = 2,
    FLOCK_WIFI_MOD    = 3,
    FLOCK_PIGVISION   = 4,
    FLOCK_MFR_MATCH   = 5,
    FLOCK_SOUNDTHINK  = 6,
    FLOCK_DIRECT_REG  = 7,
};

#define FY_MATCH_OUI   0x01
#define FY_MATCH_NAME  0x02
#define FY_MATCH_MFR   0x04
#define FY_MATCH_SVC   0x08

static const uint8_t FY_BLE_OUI[][3] PROGMEM = {
    {0xEC, 0x1B, 0xBD}, {0x58, 0x8E, 0x81}, {0x90, 0x35, 0xEA},
    {0xCC, 0xCC, 0xCC}, {0xB4, 0xE3, 0xF9}, {0x04, 0x0D, 0x84},
    {0xF0, 0x82, 0xC0}, {0x1C, 0x34, 0xF1}, {0x38, 0x5B, 0x44},
    {0x94, 0x34, 0x69},
};
#define FY_BLE_OUI_COUNT 10

static const uint8_t FY_WIFI_OUI[][3] PROGMEM = {
    {0xD8, 0xF3, 0xBC}, {0x74, 0x4C, 0xA1}, {0x14, 0x5A, 0xFC},
    {0xE4, 0xAA, 0xEA}, {0x3C, 0x91, 0x80}, {0x80, 0x30, 0x49},
    {0x08, 0x3A, 0x88}, {0x70, 0xC9, 0x4E}, {0x9C, 0x2F, 0x9D},
    {0x94, 0x08, 0x53},
};
#define FY_WIFI_OUI_COUNT 10

static const uint8_t FY_DIRECT_OUI[][3] PROGMEM = {
    {0xB4, 0x1E, 0x52},
};
#define FY_DIRECT_OUI_COUNT 1

static const uint8_t FY_SOUNDTHINK_OUI[][3] PROGMEM = {
    {0xD4, 0x11, 0xD6},
};
#define FY_SOUNDTHINK_OUI_COUNT 1

#define FY_MFR_XUNTONG 0x09C8

// Raven GATT service UUIDs
static BLEUUID FY_UUID_DEVINFO((uint16_t)0x180A);
static BLEUUID FY_UUID_GPS((uint16_t)0x3100);
static BLEUUID FY_UUID_POWER((uint16_t)0x3200);
static BLEUUID FY_UUID_NETWORK((uint16_t)0x3300);
static BLEUUID FY_UUID_UPLOAD((uint16_t)0x3400);
static BLEUUID FY_UUID_FAILURE((uint16_t)0x3500);
static BLEUUID FY_UUID_OLD_HEALTH((uint16_t)0x1809);
static BLEUUID FY_UUID_OLD_LOCATION((uint16_t)0x1819);

// Raven svcMask bits — any non-DEVINFO bit elevates classification to RAVEN.
#define FY_RAVEN_DEVINFO   0x01
#define FY_RAVEN_GPS       0x02
#define FY_RAVEN_POWER     0x04
#define FY_RAVEN_NETWORK   0x08
#define FY_RAVEN_UPLOAD    0x10
#define FY_RAVEN_FAILURE   0x20
#define FY_RAVEN_OLD_HLTH  0x40
#define FY_RAVEN_OLD_LOC   0x80
#define FY_RAVEN_HIT_MASK  (FY_RAVEN_GPS | FY_RAVEN_POWER | FY_RAVEN_NETWORK | \
                            FY_RAVEN_UPLOAD | FY_RAVEN_FAILURE |              \
                            FY_RAVEN_OLD_HLTH | FY_RAVEN_OLD_LOC)

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

struct FlockDevice {
    uint8_t  mac[6];
    int8_t   rssi;
    int8_t   bestRssi;
    uint8_t  type;
    uint8_t  matchFlags;
    char     name[20];
    uint32_t lastSeen;
    uint8_t  seenCount;
};

static FlockDevice s_devices[FY_MAX_DEVICES];
static int         s_deviceCount = 0;

// Display snapshot (built on worker, read on orchestrator).
static char s_resultBuf[FY_MAX_DEVICES][FY_RESULT_LINE_LEN];
static const char *s_resultPtrs[FY_MAX_DEVICES];
static int  s_resultCount = 0;

static volatile int s_camCount = 0;
static volatile int s_ravenCount = 0;

static TaskHandle_t      s_workerHandle = NULL;
static QueueHandle_t     s_cmdQueue     = NULL;
static SemaphoreHandle_t s_doneSem      = NULL;

// Advertisement -> worker SPSC ring (same shape as ble_scan.cpp).
// Service-UUID comparison is done in the callback with BLEUUID::equals()
// — same approach as bluetooth_attacks.cpp's FlockYou — and the result is
// condensed into svcMask bits so the ring entry stays small/POD.
#define FY_RING_SIZE 16
struct FyRingEntry {
    uint8_t mac[6];
    int8_t  rssi;
    char    name[20];
    bool    hasName;
    uint8_t mfgData[8];
    uint8_t mfgLen;
    uint8_t svcMask;   // Raven GATT service bitmask (see FY_RAVEN_* below)
};
static FyRingEntry s_ring[FY_RING_SIZE];
static volatile uint8_t  s_ringHead = 0;
static volatile uint8_t  s_ringTail = 0;
static volatile uint32_t s_ringDropped = 0;

static volatile bool s_scanWindowEnded = false;
static bool s_classicBtReleased = false;

static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) s_classicBtReleased = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// ADV CALLBACK
// ═══════════════════════════════════════════════════════════════════════════

class FyScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        uint8_t next = (s_ringHead + 1) & (FY_RING_SIZE - 1);
        if (next == s_ringTail) { s_ringDropped++; return; }

        FyRingEntry *e = &s_ring[s_ringHead];
        const uint8_t *addr = *advertisedDevice.getAddress().getNative();
        memcpy(e->mac, addr, 6);
        e->rssi = (int8_t)advertisedDevice.getRSSI();

        if (advertisedDevice.haveName() && advertisedDevice.getName().length() > 0) {
            strncpy(e->name, advertisedDevice.getName().c_str(), 19);
            e->name[19] = '\0';
            e->hasName = true;
        } else {
            e->name[0] = '\0';
            e->hasName = false;
        }

        if (advertisedDevice.haveManufacturerData()) {
            std::string mfg = advertisedDevice.getManufacturerData();
            e->mfgLen = (mfg.length() > 8) ? 8 : (uint8_t)mfg.length();
            memcpy(e->mfgData, mfg.data(), e->mfgLen);
        } else {
            e->mfgLen = 0;
        }

        e->svcMask = 0;
        if (advertisedDevice.haveServiceUUID()) {
            int n = advertisedDevice.getServiceUUIDCount();
            for (int i = 0; i < n; i++) {
                BLEUUID u = advertisedDevice.getServiceUUID(i);
                if      (u.equals(FY_UUID_DEVINFO))      e->svcMask |= FY_RAVEN_DEVINFO;
                else if (u.equals(FY_UUID_GPS))          e->svcMask |= FY_RAVEN_GPS;
                else if (u.equals(FY_UUID_POWER))        e->svcMask |= FY_RAVEN_POWER;
                else if (u.equals(FY_UUID_NETWORK))      e->svcMask |= FY_RAVEN_NETWORK;
                else if (u.equals(FY_UUID_UPLOAD))       e->svcMask |= FY_RAVEN_UPLOAD;
                else if (u.equals(FY_UUID_FAILURE))      e->svcMask |= FY_RAVEN_FAILURE;
                else if (u.equals(FY_UUID_OLD_HEALTH))   e->svcMask |= FY_RAVEN_OLD_HLTH;
                else if (u.equals(FY_UUID_OLD_LOCATION)) e->svcMask |= FY_RAVEN_OLD_LOC;
            }
        }

        s_ringHead = next;
    }
};
static FyScanCallbacks s_callbacks;

static void onScanWindowEnded(BLEScanResults results) {
    (void)results;
    s_scanWindowEnded = true;
}

// ═══════════════════════════════════════════════════════════════════════════
// CLASSIFIER HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static bool fyMatchOUI(const uint8_t *mac, const uint8_t table[][3], int count) {
    for (int i = 0; i < count; i++) {
        uint8_t oui[3];
        memcpy_P(oui, table[i], 3);
        if (mac[0] == oui[0] && mac[1] == oui[1] && mac[2] == oui[2]) return true;
    }
    return false;
}

static bool fyNameContains(const char *hay, const char *needle) {
    if (!hay || !needle || needle[0] == '\0') return false;
    int hLen = strlen(hay);
    int nLen = strlen(needle);
    if (nLen > hLen) return false;
    for (int i = 0; i <= hLen - nLen; i++) {
        bool m = true;
        for (int j = 0; j < nLen; j++) {
            if (tolower((unsigned char)hay[i+j]) != tolower((unsigned char)needle[j])) { m = false; break; }
        }
        if (m) return true;
    }
    return false;
}

static int findDeviceIndex(const uint8_t *mac) {
    for (int i = 0; i < s_deviceCount; i++) {
        if (memcmp(s_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static const char *typeLabel(uint8_t t) {
    switch (t) {
        case FLOCK_EXT_BATT:   return "BAT";
        case FLOCK_RAVEN:      return "RAV";
        case FLOCK_PENGUIN:    return "PEN";
        case FLOCK_WIFI_MOD:   return "WFI";
        case FLOCK_PIGVISION:  return "PIG";
        case FLOCK_MFR_MATCH:  return "XUN";
        case FLOCK_SOUNDTHINK: return "SST";
        case FLOCK_DIRECT_REG: return "FLK";
        default:               return "UNK";
    }
}

static void classifyAndStore(const FyRingEntry *e) {
    uint8_t matchFlags = 0;
    uint8_t devType = FLOCK_MFR_MATCH;

    // 1. OUI tables (4)
    if      (fyMatchOUI(e->mac, FY_BLE_OUI, FY_BLE_OUI_COUNT))               { matchFlags |= FY_MATCH_OUI; devType = FLOCK_EXT_BATT; }
    else if (fyMatchOUI(e->mac, FY_WIFI_OUI, FY_WIFI_OUI_COUNT))             { matchFlags |= FY_MATCH_OUI; devType = FLOCK_WIFI_MOD; }
    else if (fyMatchOUI(e->mac, FY_DIRECT_OUI, FY_DIRECT_OUI_COUNT))         { matchFlags |= FY_MATCH_OUI; devType = FLOCK_DIRECT_REG; }
    else if (fyMatchOUI(e->mac, FY_SOUNDTHINK_OUI, FY_SOUNDTHINK_OUI_COUNT)) { matchFlags |= FY_MATCH_OUI; devType = FLOCK_SOUNDTHINK; }

    // 2. Name match (case-insensitive)
    if (e->hasName) {
        if      (fyNameContains(e->name, "FS Ext Battery")) { matchFlags |= FY_MATCH_NAME; devType = FLOCK_EXT_BATT; }
        else if (fyNameContains(e->name, "Penguin"))        { matchFlags |= FY_MATCH_NAME; devType = FLOCK_PENGUIN; }
        else if (fyNameContains(e->name, "Pigvision"))      { matchFlags |= FY_MATCH_NAME; devType = FLOCK_PIGVISION; }
        else if (fyNameContains(e->name, "Flock")) {
            matchFlags |= FY_MATCH_NAME;
            if (devType == FLOCK_MFR_MATCH) devType = FLOCK_WIFI_MOD;
        }
    }

    // 3. Manufacturer ID (XUNTONG)
    if (e->mfgLen >= 2) {
        uint16_t cid = (uint16_t)e->mfgData[0] | ((uint16_t)e->mfgData[1] << 8);
        if (cid == FY_MFR_XUNTONG) matchFlags |= FY_MATCH_MFR;
    }

    // 4. Service UUID (Raven GATT) — DeviceInfo alone doesn't elevate.
    if (e->svcMask & FY_RAVEN_HIT_MASK) {
        matchFlags |= FY_MATCH_SVC;
        if (devType != FLOCK_PENGUIN) devType = FLOCK_RAVEN;
    }

    if (matchFlags == 0) return;

    int idx = findDeviceIndex(e->mac);
    if (idx >= 0) {
        FlockDevice *d = &s_devices[idx];
        d->rssi = e->rssi;
        if (e->rssi > d->bestRssi) d->bestRssi = e->rssi;
        d->lastSeen = millis();
        if (d->seenCount < 255) d->seenCount++;
        d->matchFlags |= matchFlags;
        if (devType == FLOCK_RAVEN || devType == FLOCK_PENGUIN) d->type = devType;
        if (e->hasName && d->name[0] == '\0') {
            strncpy(d->name, e->name, 19);
            d->name[19] = '\0';
        }
        return;
    }

    if (s_deviceCount >= FY_MAX_DEVICES) return;
    FlockDevice *d = &s_devices[s_deviceCount++];
    memset(d, 0, sizeof(*d));
    memcpy(d->mac, e->mac, 6);
    d->rssi = d->bestRssi = e->rssi;
    d->lastSeen = millis();
    d->seenCount = 1;
    d->type = devType;
    d->matchFlags = matchFlags;
    if (e->hasName) { strncpy(d->name, e->name, 19); d->name[19] = '\0'; }
}

static void pruneStale() {
    uint32_t now = millis();
    int i = 0;
    while (i < s_deviceCount) {
        if (now - s_devices[i].lastSeen > FY_STALE_TIMEOUT) {
            for (int j = i; j < s_deviceCount - 1; j++) s_devices[j] = s_devices[j+1];
            s_deviceCount--;
        } else {
            i++;
        }
    }
}

static void formatResults() {
    int cam = 0, rav = 0;
    int n = 0;
    for (int i = 0; i < s_deviceCount && n < FY_MAX_DEVICES; i++) {
        FlockDevice *d = &s_devices[i];
        switch (d->type) {
            case FLOCK_EXT_BATT: case FLOCK_WIFI_MOD:
            case FLOCK_DIRECT_REG: case FLOCK_MFR_MATCH:
            case FLOCK_PIGVISION:
                cam++; break;
            case FLOCK_RAVEN: case FLOCK_PENGUIN: case FLOCK_SOUNDTHINK:
                rav++; break;
        }

        char dev[20];
        if (d->name[0]) {
            strncpy(dev, d->name, 16);
            dev[16] = '\0';
        } else {
            snprintf(dev, sizeof(dev), "%02X:%02X:%02X",
                     d->mac[3], d->mac[4], d->mac[5]);
        }
        snprintf(s_resultBuf[n], FY_RESULT_LINE_LEN, "%s %s %ddBm",
                 typeLabel(d->type), dev, (int)d->rssi);
        s_resultPtrs[n] = s_resultBuf[n];
        n++;
    }
    s_camCount = cam;
    s_ravenCount = rav;
    if (n == 0) {
        snprintf(s_resultBuf[0], FY_RESULT_LINE_LEN, "(scanning... no matches)");
        s_resultPtrs[0] = s_resultBuf[0];
        n = 1;
    }
    s_resultCount = n;
}

static void postRefresh() {
    formatResults();
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE;
    messagingPostTelemetry(msg);
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER
// ═══════════════════════════════════════════════════════════════════════════

static void flockYouWorkerTask(void *param) {
    (void)param;

    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150);

    BLEScan *pScan = BLEDevice::getScan();
    if (!pScan) {
        BLEDevice::deinit(false);
        WiFi.mode(WIFI_STA);
        radioRelease(RADIO_BLE);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    pScan->setActiveScan(false); // passive — stealth
    pScan->setInterval(100);
    pScan->setWindow(99);
    pScan->setAdvertisedDeviceCallbacks(&s_callbacks, true);

    s_deviceCount = 0;
    s_resultCount = 0;
    s_camCount = 0;
    s_ravenCount = 0;
    s_ringHead = 0;
    s_ringTail = 0;
    s_ringDropped = 0;
    s_scanWindowEnded = false;

    // Initial list render so the orchestrator can flip IDLE->RUNNING.
    formatResults();
    postRefresh();

    pScan->start(FY_SCAN_WINDOW_SEC, onScanWindowEnded, false);

    bool dirty = false;
    uint32_t lastRefresh = millis();
    uint32_t lastPrune = millis();

    for (;;) {
        // Drain the ring.
        while (s_ringTail != s_ringHead) {
            FyRingEntry e;
            memcpy(&e, (const void *)&s_ring[s_ringTail], sizeof(e));
            s_ringTail = (s_ringTail + 1) & (FY_RING_SIZE - 1);
            classifyAndStore(&e);
            dirty = true;
        }

        if (s_scanWindowEnded) {
            s_scanWindowEnded = false;
            pScan->start(FY_SCAN_WINDOW_SEC, onScanWindowEnded, false);
        }

        uint32_t now = millis();
        if (now - lastPrune > 5000) {
            lastPrune = now;
            int before = s_deviceCount;
            pruneStale();
            if (s_deviceCount != before) dirty = true;
        }

        if (dirty && (now - lastRefresh >= FY_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            postRefresh();
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (cmd.type == CMD_STOP) {
                pScan->stop();
                break;
            }
        }
    }

    pScan->setAdvertisedDeviceCallbacks(nullptr, false);
    BLEDevice::deinit(false);
    WiFi.mode(WIFI_STA);
    radioRelease(RADIO_BLE);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void flockYouStart() {
    if (s_cmdQueue) return;
    if (!radioAcquire(RADIO_BLE)) return;

    s_resultCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem  = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(flockYouWorkerTask, "flock_you", 6144,
                            NULL, 2, &s_workerHandle, 0);
}

void flockYouStop() {
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

const char *const *flockYouGetResultLines() { return s_resultPtrs; }
int flockYouGetResultCount() { return s_resultCount; }
int flockYouGetDeviceCount() { return s_deviceCount; }
int flockYouGetCamCount()    { return s_camCount; }
int flockYouGetRavenCount()  { return s_ravenCount; }
