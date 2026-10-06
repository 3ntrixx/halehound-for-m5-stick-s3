// ═══════════════════════════════════════════════════════════════════════════
// HaleHound BLE Scanner — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 5)
// Rewritten 2026-09-18 for fidelity with bluetooth_attacks.cpp's BlePredator
// PHASE_LISTEN behavior (per user direction: "reproduce anything halehound
// does... as much as you possibly can"). The original's LISTEN phase is a
// continuous passive scan — 5s scan windows auto-chained forever via the
// completion callback (bpScanComplete), devices deduped by MAC and updated
// in place (RSSI/min/max, frame count, first/last seen) on every repeat
// sighting, name/payload/manufacturer data merged in as later frames carry
// it, each device classified RED/YELLOW/SKIP by BlePredator's own
// classification tables (ble_database.h), SKIP-tier devices always hidden,
// and the visible list filtered by the ALL/NAMED/STRONG/HIGH modes —
// display refreshed at most once/sec while dirty.
//
// Row format maps the original's three-column row (RSSI bar | DEVICE |
// WHAT) to one text line: "[!] <name or XX:XX:XX> <appearance/company>
// <rssi>dBm[*]" where "!" marks a RED-tier target and "*" marks an active
// device (original's active dot: >5 frames and seen <10s ago).
//
// Device cap is MAX_RESULTS=62, same as the original's BP_MAX_DEVICES.
// ═══════════════════════════════════════════════════════════════════════════

#include "ble_scan.h"
#include <Arduino.h>
#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_bt.h>
#include "ble_database.h"
#include "skeletonkey_protocol.h" // name-matched "Light" tag in deviceWhat()
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "ui_render.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define MAX_RESULTS 62             // matches BlePredator's BP_MAX_DEVICES
#define RESULT_LINE_LEN 52         // ~312px at text size 1 on this 320px panel
#define BLE_SCAN_WINDOW_SEC 5      // one scan window; auto-chained for continuous listening
#define BLE_REFRESH_MIN_MS  1000   // matches original's listenDirty redraw throttle
#define BLE_ACTIVE_FRAMES   5      // original's active dot: frameCount > 5 ...
#define BLE_ACTIVE_AGE_MS   10000  // ... and last seen < 10s ago

static char s_resultBuf[MAX_RESULTS][RESULT_LINE_LEN];
static const char *s_resultPtrs[MAX_RESULTS];
static int s_resultCount = 0;

// Display snapshot for ui_render's Predator screen — built by the worker in
// formatResults() (sole owner of s_devices[]) and copied out by
// bleScanGetDisplayRows() from the orchestrator. s_displayMac/AddrType are
// the same filtered-index[n] shape, added for SkeletonKey's Predator-handoff
// (v3.9.x port plan Phase 11) — BleDisplayRow itself only carries cosmetic
// text, not the raw address a BLEClient::connect() actually needs.
static BleDisplayRow s_displayRows[MAX_RESULTS];
static uint8_t s_displayMac[MAX_RESULTS][6];
static uint8_t s_displayAddrType[MAX_RESULTS];
static volatile int s_displayCount = 0;
static volatile int s_filteredCount = 0;
static volatile uint32_t s_scanStartMs = 0;

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Device table (dedup by MAC, insertion order — original doesn't sort
// its list either) — mirrors BlePredator's ReconDevice ──────────────────
struct BleDevice {
    uint8_t mac[6];
    uint8_t addrType;
    char name[17];
    bool hasName;
    uint8_t payload[31];
    uint8_t payloadLen;
    uint8_t mfgData[8];
    uint8_t mfgLen;
    uint16_t companyId;
    int8_t rssi;
    int8_t rssiMin;
    int8_t rssiMax;
    uint16_t frameCount;
    uint32_t firstSeen;
    uint32_t lastSeen;
    bool randomMAC;
    uint8_t tier;
};
static BleDevice s_devices[MAX_RESULTS];
static int s_deviceCount = 0;

// Filter modes — same set and semantics as BlePredator's PredFilter.
enum BleFilter : uint8_t { BF_ALL = 0, BF_NAMED, BF_STRONG, BF_HIGH, BF_COUNT };
static const char *const BF_NAMES[] = {"ALL", "NAMED", "STRONG", "HIGH"};
static volatile uint8_t s_filter = BF_ALL;

// Tier counts for the header, written by the worker (sole owner of
// s_devices[]) and read by the orchestrator.
static volatile int s_redCount = 0;
static volatile int s_yellowCount = 0;

// ESP32 Bluedroid reserves ~28KB for Classic BT by default. HaleHound is
// BLE-5.0-only on this board (plan §2) — release that memory once, before
// the first BLEDevice::init() triggers esp_bt_controller_init(). Mirrors
// bluetooth_attacks.cpp's releaseClassicBtMemory().
static bool s_classicBtReleased = false;
static void releaseClassicBtMemory() {
    if (s_classicBtReleased) return;
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err == ESP_OK) {
        s_classicBtReleased = true;
    }
    #if CYD_DEBUG
    Serial.printf("[BLE_SCAN] releaseClassicBtMemory: 0x%x, heap=%u\n", err, ESP.getFreeHeap());
    #endif
}

// ── Advertisement callback → 16-slot SPSC ring buffer ───────────────────
// BlePredator's loop drains its single pending slot every loop() iteration;
// this worker can't run that often, so the single slot dropped nearly every
// frame in a busy area. The ring stages full frames (name/mfg/raw ADV
// payload, the same fields the original's PredatorCallbacks copies) and the
// worker drains all of it each iteration.
#define BLE_RING_SIZE 16   // power of two
struct BleRingEntry {
    uint8_t mac[6];
    uint8_t addrType;
    int8_t rssi;
    char name[17];
    bool hasName;
    uint8_t mfgData[8];
    uint8_t mfgLen;
    uint8_t payload[31];
    uint8_t payloadLen;
};
static BleRingEntry s_ring[BLE_RING_SIZE];
static volatile uint8_t s_ringHead = 0;   // producer: BLE callback
static volatile uint8_t s_ringTail = 0;   // consumer: worker
static volatile uint32_t s_ringDropped = 0;

class BleScanCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) override {
        uint8_t next = (s_ringHead + 1) & (BLE_RING_SIZE - 1);
        if (next == s_ringTail) {
            s_ringDropped++;
            return;
        }

        BleRingEntry *e = &s_ring[s_ringHead];

        const uint8_t *addr = *advertisedDevice.getAddress().getNative();
        memcpy(e->mac, addr, 6);
        e->addrType = advertisedDevice.getAddressType();
        e->rssi = advertisedDevice.getRSSI();

        if (advertisedDevice.haveName() && advertisedDevice.getName().length() > 0) {
            strncpy(e->name, advertisedDevice.getName().c_str(), 16);
            e->name[16] = '\0';
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

        // Full raw ADV payload — the original stores this for its
        // appearance/service-UUID classification and replay phase.
        uint8_t *pld = advertisedDevice.getPayload();
        size_t pldLen = advertisedDevice.getPayloadLength();
        if (pld && pldLen > 0) {
            e->payloadLen = (pldLen > 31) ? 31 : (uint8_t)pldLen;
            memcpy(e->payload, pld, e->payloadLen);
        } else {
            e->payloadLen = 0;
        }

        s_ringHead = next;
    }
};
static BleScanCallbacks s_callbacks;

static volatile bool s_scanWindowEnded = false;
static void onScanWindowEnded(BLEScanResults results) {
    (void)results;
    s_scanWindowEnded = true; // task loop restarts the next window — see bpScanComplete
}

// ═══════════════════════════════════════════════════════════════════════════
// ADV PARSER + CLASSIFICATION — ported from bluetooth_attacks.cpp's
// bpParseAdvPayload()/bpClassifyDevice()/bpNameContains()
// ═══════════════════════════════════════════════════════════════════════════

struct AdvParsed {
    uint16_t svcUUIDs[4];
    uint16_t appearance;   // 0xFFFF = absent
    uint8_t svcCount;
    uint8_t flags;
    bool hasFlags;
};

static void parseAdvPayload(const uint8_t *payload, uint8_t len, AdvParsed *out) {
    out->svcCount = 0;
    out->appearance = 0xFFFF;
    out->flags = 0;
    out->hasFlags = false;

    uint8_t pos = 0;
    while (pos < len) {
        uint8_t adLen = payload[pos];
        if (adLen == 0 || pos + adLen >= len) break;
        uint8_t adType = payload[pos + 1];
        const uint8_t *data = &payload[pos + 2];
        uint8_t dataLen = adLen - 1;

        switch (adType) {
            case 0x01: // Flags
                if (dataLen >= 1) {
                    out->flags = data[0];
                    out->hasFlags = true;
                }
                break;
            case 0x02: // Incomplete 16-bit service UUIDs
            case 0x03: // Complete 16-bit service UUIDs
                for (uint8_t i = 0; i + 1 < dataLen && out->svcCount < 4; i += 2) {
                    out->svcUUIDs[out->svcCount++] = data[i] | (data[i + 1] << 8);
                }
                break;
            case 0x19: // Appearance
                if (dataLen >= 2) {
                    out->appearance = data[0] | (data[1] << 8);
                }
                break;
        }
        pos += adLen + 1;
    }
}

// Case-insensitive substring search (no stdlib dependency)
static bool nameContains(const char *name, const char *sub) {
    if (!name[0] || !sub[0]) return false;
    int nLen = strlen(name);
    int sLen = strlen(sub);
    if (sLen > nLen) return false;
    for (int i = 0; i <= nLen - sLen; i++) {
        bool match = true;
        for (int j = 0; j < sLen; j++) {
            char a = name[i + j];
            char b = sub[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Classify device into RED / SKIP / YELLOW tier — verbatim port of
// bpClassifyDevice() using the same tables from ble_database.h.
static uint8_t classifyDevice(BleDevice *d) {
    AdvParsed adv;
    parseAdvPayload(d->payload, d->payloadLen, &adv);

    // ── Priority 1-3: RED by service UUID, appearance, company ID ────────
    for (uint8_t i = 0; i < adv.svcCount; i++) {
        if (inProgmemU16List(adv.svcUUIDs[i], redServiceUUIDs, RED_SVC_COUNT))
            return TIER_RED;
    }
    if (adv.appearance != 0xFFFF) {
        if (inProgmemU16List(adv.appearance, redAppearances, RED_APP_COUNT))
            return TIER_RED;
    }
    if (d->companyId != 0xFFFF) {
        if (inProgmemU16List(d->companyId, redCompanyIDs, RED_CID_COUNT))
            return TIER_RED;
    }

    // ── Priority 4: RED by device name patterns ──────────────────────────
    if (d->hasName) {
        static const char *redNames[] = {
            "lock", "safe", "door", "key", "access",
            "keyboard", "provision", "gateway"
        };
        for (int i = 0; i < 8; i++) {
            if (nameContains(d->name, redNames[i])) return TIER_RED;
        }
    }

    // ── Priority 5: RED if in pairing mode (LE Limited Discoverable) ─────
    if (adv.hasFlags && (adv.flags & 0x01)) return TIER_RED;

    // ── Priority 6-8: SKIP by service UUID, appearance, company ID ───────
    for (uint8_t i = 0; i < adv.svcCount; i++) {
        if (inProgmemU16List(adv.svcUUIDs[i], skipServiceUUIDs, SKIP_SVC_COUNT))
            return TIER_SKIP;
    }
    if (adv.appearance != 0xFFFF) {
        if (inProgmemU16List(adv.appearance, skipAppearances, SKIP_APP_COUNT))
            return TIER_SKIP;
    }
    if (d->companyId != 0xFFFF) {
        if (inProgmemU16List(d->companyId, skipCompanyIDs, SKIP_CID_COUNT))
            return TIER_SKIP;
    }

    // ── Priority 9: SKIP by device name patterns ─────────────────────────
    if (d->hasName) {
        static const char *skipNames[] = {
            "buds", "pods", "airpod", "speaker", "soundbar",
            "headphone", "earbud", "band", "watch", "fitbit",
            "garmin", "polar", "oura", "beacon", "tile",
            "tag", "gamepad", "controller", "joycon"
        };
        for (int i = 0; i < 19; i++) {
            if (nameContains(d->name, skipNames[i])) return TIER_SKIP;
        }
    }

    // ── Priority 10: Default yellow ──────────────────────────────────────
    return TIER_YELLOW;
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE TABLE MANAGEMENT — ported from bpFindDevice()/bpAddOrUpdate()
// ═══════════════════════════════════════════════════════════════════════════

// (company-ID extraction lives in ble_database.h as extractCompanyId())

static int findDeviceIndex(const uint8_t *mac) {
    for (int i = 0; i < s_deviceCount; i++) {
        if (memcmp(s_devices[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

// WHAT column label: SkeletonKey light match > appearance detail >
// company name > "Named" > "???". The name-matched check comes first: the
// generic BLE LED strips/bulbs SkeletonKeyProtocol knows how to control
// (ELK-BLEDOM, Triones, etc.) typically advertise no GAP Appearance at all
// (or a useless Generic/0x0000), so a name match is the stronger signal —
// and it doubles as a preview of SkeletonKey's one-tap control support
// before the user ever taps in to connect.
static const char *deviceWhat(const BleDevice *d) {
    if (d->hasName) {
        for (int i = 0; i < SkeletonKeyProtocol::ProfileCount; ++i) {
            if (SkeletonKeyProtocol::matchesName(d->name, SkeletonKeyProtocol::Profiles[i].name))
                return "Light";
        }
    }
    AdvParsed adv;
    parseAdvPayload(d->payload, d->payloadLen, &adv);
    if (adv.appearance != 0xFFFF) {
        const char *detail = lookupAppearanceDetail(adv.appearance);
        if (detail) return detail;
    }
    if (d->companyId != 0xFFFF) {
        const char *co = lookupCompanyName(d->companyId);
        if (co) return co;
    }
    return d->hasName ? "Named" : "???";
}

static bool addOrUpdate(const BleRingEntry *e) {
    uint32_t now = millis();
    int idx = findDeviceIndex(e->mac);

    if (idx >= 0) {
        BleDevice *d = &s_devices[idx];
        d->rssi = e->rssi;
        if (e->rssi < d->rssiMin) d->rssiMin = e->rssi;
        if (e->rssi > d->rssiMax) d->rssiMax = e->rssi;
        d->lastSeen = now;
        d->frameCount++;

        bool dataChanged = false;
        if (e->hasName && !d->hasName) {
            strncpy(d->name, e->name, 16);
            d->name[16] = '\0';
            d->hasName = true;
            dataChanged = true;
        }
        if (e->payloadLen > d->payloadLen) {
            memcpy(d->payload, e->payload, e->payloadLen);
            d->payloadLen = e->payloadLen;
            dataChanged = true;
        }
        if (e->mfgLen > 0 && d->mfgLen == 0) {
            uint8_t copyLen = (e->mfgLen > 8) ? 8 : e->mfgLen;
            memcpy(d->mfgData, e->mfgData, copyLen);
            d->mfgLen = copyLen;
            uint16_t cid = extractCompanyId(e->mfgData, e->mfgLen);
            if (cid != 0xFFFF) d->companyId = cid;
            dataChanged = true;
        }
        // Re-classify if new data could change the tier
        if (dataChanged && d->tier == TIER_YELLOW) {
            d->tier = classifyDevice(d);
        }
        return true;
    }

    if (s_deviceCount >= MAX_RESULTS) return false;

    BleDevice *d = &s_devices[s_deviceCount];
    memset(d, 0, sizeof(*d));
    memcpy(d->mac, e->mac, 6);
    d->addrType = e->addrType;
    d->rssi = d->rssiMin = d->rssiMax = e->rssi;
    d->firstSeen = d->lastSeen = now;
    d->frameCount = 1;
    d->randomMAC = (e->mac[0] & 0x02) != 0;

    if (e->payloadLen > 0) {
        d->payloadLen = (e->payloadLen > 31) ? 31 : e->payloadLen;
        memcpy(d->payload, e->payload, d->payloadLen);
    }
    if (e->hasName) {
        strncpy(d->name, e->name, 16);
        d->name[16] = '\0';
        d->hasName = true;
    }
    if (e->mfgLen > 0) {
        d->mfgLen = (e->mfgLen > 8) ? 8 : e->mfgLen;
        memcpy(d->mfgData, e->mfgData, d->mfgLen);
        d->companyId = extractCompanyId(e->mfgData, e->mfgLen);
    } else {
        d->companyId = 0xFFFF;
    }

    d->tier = classifyDevice(d);
    s_deviceCount++;

    #if CYD_DEBUG
    static const char *tierTags[] = {"YEL", "RED", "SKIP"};
    Serial.printf("[BLE_SCAN] +DEV #%d %02X:%02X:%02X:%02X:%02X:%02X %ddBm pld:%d [%s]\n",
                  s_deviceCount, e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                  e->rssi, d->payloadLen, tierTags[d->tier]);
    #endif
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
// FILTER + RESULT FORMATTING — ported from bpPassesFilter() and the
// drawDeviceList() row layout
// ═══════════════════════════════════════════════════════════════════════════

static bool passesFilter(const BleDevice *d) {
    // SKIP-tier devices are ALWAYS hidden regardless of filter mode
    if (d->tier == TIER_SKIP) return false;
    switch (s_filter) {
        case BF_NAMED:  return d->hasName;
        case BF_STRONG: return d->rssi > -60;
        case BF_HIGH:   return d->tier == TIER_RED;
        default:        return true;
    }
}

static void formatDeviceRow(char *out, size_t outLen, const BleDevice *d, uint32_t now) {
    char dev[20];
    if (d->hasName && d->name[0]) {
        strncpy(dev, d->name, 18);
        dev[18] = '\0';
    } else {
        snprintf(dev, sizeof(dev), "%02X:%02X:%02X", d->mac[3], d->mac[4], d->mac[5]);
    }

    bool active = (d->frameCount > BLE_ACTIVE_FRAMES) &&
                  ((now - d->lastSeen) < BLE_ACTIVE_AGE_MS);

    snprintf(out, outLen, "%s%s %s %ddBm%c",
             (d->tier == TIER_RED) ? "! " : "", dev, deviceWhat(d), d->rssi,
             active ? '*' : ' ');
}

static void formatResults() {
    uint32_t now = millis();
    int n = 0;
    int red = 0, yellow = 0;

    for (int i = 0; i < s_deviceCount && n < MAX_RESULTS; i++) {
        BleDevice *d = &s_devices[i];
        if (d->tier == TIER_RED) red++;
        else if (d->tier == TIER_YELLOW) yellow++;
        if (!passesFilter(d)) continue;

        formatDeviceRow(s_resultBuf[n], RESULT_LINE_LEN, d, now);
        s_resultPtrs[n] = s_resultBuf[n];

        memcpy(s_displayMac[n], d->mac, 6);
        s_displayAddrType[n] = d->addrType;

        BleDisplayRow *r = &s_displayRows[n];
        if (d->hasName && d->name[0]) {
            strncpy(r->device, d->name, sizeof(r->device) - 1);
            r->device[sizeof(r->device) - 1] = '\0';
        } else {
            snprintf(r->device, sizeof(r->device), "%02X:%02X:%02X",
                     d->mac[3], d->mac[4], d->mac[5]);
        }
        strncpy(r->what, deviceWhat(d), sizeof(r->what) - 1);
        r->what[sizeof(r->what) - 1] = '\0';
        r->rssi = d->rssi;
        r->tier = d->tier;
        r->hasName = d->hasName;
        r->companyKnown = (d->companyId != 0xFFFF && lookupCompanyName(d->companyId) != nullptr);
        r->active = (d->frameCount > BLE_ACTIVE_FRAMES) && ((now - d->lastSeen) < BLE_ACTIVE_AGE_MS);
        r->stale = ((now - d->lastSeen) > 30000);
        n++;
    }

    s_redCount = red;
    s_yellowCount = yellow;
    s_filteredCount = n;
    s_displayCount = n;

    if (n == 0) {
        snprintf(s_resultBuf[0], RESULT_LINE_LEN, "(no devices%s)",
                 (s_filter == BF_ALL) ? " found" : " in filter");
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

static void bleScanWorkerTask(void *param) {
    (void)param;

    // Tear down WiFi before BLE init — frees ~50KB heap (matches
    // bluetooth_attacks.cpp's BleScan::setup()/cleanup()).
    WiFi.mode(WIFI_OFF);
    delay(50);

    releaseClassicBtMemory();
    BLEDevice::init("");
    delay(150); // BLE controller needs time after init before first scan

    BLEScan *pBleScan = BLEDevice::getScan();
    if (!pBleScan) {
        #if CYD_DEBUG
        Serial.println("[BLE_SCAN] ERROR: getScan() returned NULL");
        #endif
        BLEDevice::deinit(false);
        WiFi.mode(WIFI_STA);
        radioRelease(RADIO_BLE);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    pBleScan->setActiveScan(false); // passive — stealth mode, matches BlePredator's LISTEN phase
    pBleScan->setInterval(100);
    pBleScan->setWindow(99); // near-continuous listening
    pBleScan->setAdvertisedDeviceCallbacks(&s_callbacks, true); // wantDuplicates=true — repeat sightings update RSSI live

    s_deviceCount = 0;
    s_filter = BF_ALL;
    s_redCount = 0;
    s_yellowCount = 0;
    s_displayCount = 0;
    s_filteredCount = 0;
    s_scanStartMs = millis();
    s_ringHead = 0;
    s_ringTail = 0;
    s_ringDropped = 0;
    s_scanWindowEnded = false;
    pBleScan->start(BLE_SCAN_WINDOW_SEC, onScanWindowEnded, false);

    bool transitioned = false;
    bool dirty = false;
    uint32_t lastRefresh = 0;

    for (;;) {
        // Drain every staged advertisement first — this is the hot path.
        // Copy the entry out before advancing the tail so the producer can
        // safely reuse the slot.
        while (s_ringTail != s_ringHead) {
            BleRingEntry e;
            memcpy(&e, (const void *)&s_ring[s_ringTail], sizeof(e));
            s_ringTail = (s_ringTail + 1) & (BLE_RING_SIZE - 1);
            addOrUpdate(&e);
            dirty = true;
        }

        if (s_scanWindowEnded) {
            s_scanWindowEnded = false;
            if (!transitioned) {
                // First window's results — hands control to the orchestrator
                // (NAV_BLE_SCAN_RUNNING -> NAV_BLE_SCAN_RESULTS), same ~5s
                // timing as before this rewrite.
                transitioned = true;
                dirty = false;
                lastRefresh = millis();
                postRefresh();
            }
            pBleScan->start(BLE_SCAN_WINDOW_SEC, onScanWindowEnded, false);
        }

        uint32_t now = millis();
        if (transitioned && dirty && (now - lastRefresh >= BLE_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            postRefresh();
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (cmd.type == CMD_STOP) {
                pBleScan->stop();
                break;
            }
            if (cmd.type == CMD_PAUSE) { // reused as "cycle filter" — see bleScanCycleFilter()
                s_filter = (s_filter + 1) % BF_COUNT;
                lastRefresh = 0; // refresh on the next iteration
                dirty = true;
            }
        }
    }

    pBleScan->setAdvertisedDeviceCallbacks(nullptr, false);
    BLEDevice::deinit(false); // false = keep BLE memory so reinit works
    WiFi.mode(WIFI_STA);
    radioRelease(RADIO_BLE);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void bleScanStart() {
    if (!radioAcquire(RADIO_BLE)) {
        #if CYD_DEBUG
        Serial.println("[BLE_SCAN] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_resultCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(bleScanWorkerTask, "ble_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void bleScanStop() {
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

const char *const *bleScanGetResultLines() {
    return s_resultPtrs;
}

int bleScanGetResultCount() {
    return s_resultCount;
}

const char *bleScanGetFilterName() {
    return BF_NAMES[s_filter];
}

void bleScanGetTierCounts(int *red, int *yellow) {
    if (red) *red = s_redCount;
    if (yellow) *yellow = s_yellowCount;
}

void bleScanCycleFilter() {
    if (!s_cmdQueue) return;
    Cmd cmd;
    cmd.type = CMD_PAUSE; // reused as "cycle filter" — see the worker's cmd handling
    xQueueSend(s_cmdQueue, &cmd, 0);
}

void bleScanGetDisplayRows(BleDisplayRow *out, int max, BleDisplayStats *stats) {
    if (!out || !stats || max <= 0) return;

    int n = s_displayCount;
    if (n > max) n = max;
    if (n > 0) {
        memcpy(out, (const void *)s_displayRows, n * sizeof(BleDisplayRow));
    }

    stats->count = n;
    stats->filtered = s_filteredCount;
    stats->red = s_redCount;
    stats->yellow = s_yellowCount;
    stats->total = s_deviceCount;
    stats->elapsedSec = (millis() - s_scanStartMs) / 1000;
    snprintf(stats->filterName, sizeof(stats->filterName), "%s", BF_NAMES[s_filter]);
}

bool bleScanGetTargetMac(int index, uint8_t *outMac, uint8_t *outAddrType) {
    if (!outMac || !outAddrType || index < 0 || index >= s_displayCount) return false;
    memcpy(outMac, s_displayMac[index], 6);
    *outAddrType = s_displayAddrType[index];
    return true;
}

bool bleScanGetTargetName(int index, char *outName, size_t outSize) {
    if (!outName || !outSize || index < 0 || index >= s_displayCount) return false;
    const BleDisplayRow &row = s_displayRows[index];
    if (!row.hasName) return false; // .device is a MAC fragment, not a name
    snprintf(outName, outSize, "%s", row.device);
    return true;
}
