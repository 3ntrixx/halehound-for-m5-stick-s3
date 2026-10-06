// ═══════════════════════════════════════════════════════════════════════════
// HaleHound IoT Recon — T-Embed CC1101 implementation
// Created: 2026-10-03  (see iot_recon_tembed.h for the port writeup)
// ═══════════════════════════════════════════════════════════════════════════

#include "iot_recon_tembed.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ctype.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#define IOT_MAX_DEVICES      32
#define IOT_RESULT_LINE_LEN  52
#define IOT_RESCAN_INTERVAL_MS 15000   // one full WiFi scan every 15s
#define IOT_STALE_TIMEOUT_MS   60000   // drop devices unseen for 60s

// ═══════════════════════════════════════════════════════════════════════════
// IOT CLASSIFIER TABLES
//
// SSID substrings and BSSID OUIs taken from the vendor list CYD IoT Recon
// brute-forces against (iot_recon.cpp credDict table): Hikvision, Dahua,
// Reolink, Amcrest, Foscam, Lorex, Swann, D-Link, TP-Link, Wyze, Ring,
// Nest/Google, Amazon/Alexa, Xiaomi, Samsung SmartThings, Belkin/Wemo,
// Philips Hue, Sonos, Roku, Chromecast, TP-Link Kasa.
// ═══════════════════════════════════════════════════════════════════════════

enum IotCategory : uint8_t {
    IOT_UNKNOWN  = 0,
    IOT_CAMERA   = 1,   // IP camera / NVR
    IOT_HUB      = 2,   // smart-home hub (Ring/Nest/Alexa/etc)
    IOT_PRINTER  = 3,
    IOT_TV       = 4,   // smart TV / cast dongle
    IOT_ROUTER   = 5,   // consumer home router
};

struct SsidRule {
    const char *needle;   // case-insensitive substring
    uint8_t     cat;      // IotCategory
    const char *vendor;
};

static const SsidRule SSID_RULES[] = {
    // Cameras / NVRs
    {"hik",         IOT_CAMERA, "Hikvision"},
    {"dahua",       IOT_CAMERA, "Dahua"},
    {"reolink",     IOT_CAMERA, "Reolink"},
    {"amcrest",     IOT_CAMERA, "Amcrest"},
    {"foscam",      IOT_CAMERA, "Foscam"},
    {"lorex",       IOT_CAMERA, "Lorex"},
    {"swann",       IOT_CAMERA, "Swann"},
    {"xmeye",       IOT_CAMERA, "XMEye"},
    {"ipcam",       IOT_CAMERA, "IPCam"},
    {"nvr",         IOT_CAMERA, "NVR"},
    {"wyzecam",     IOT_CAMERA, "Wyze Cam"},
    {"wyze_cam",    IOT_CAMERA, "Wyze Cam"},

    // Smart-home hubs & assistants
    {"wyze",        IOT_HUB,    "Wyze"},
    {"ring",        IOT_HUB,    "Ring"},
    {"nest",        IOT_HUB,    "Nest"},
    {"alexa",       IOT_HUB,    "Alexa"},
    {"echo",        IOT_HUB,    "Amazon Echo"},
    {"google-home", IOT_HUB,    "Google Home"},
    {"googlehome",  IOT_HUB,    "Google Home"},
    {"smartthings", IOT_HUB,    "SmartThings"},
    {"smart-things",IOT_HUB,    "SmartThings"},
    {"hue",         IOT_HUB,    "Philips Hue"},
    {"kasa",        IOT_HUB,    "TP-Link Kasa"},
    {"wemo",        IOT_HUB,    "Belkin Wemo"},
    {"sonos",       IOT_HUB,    "Sonos"},
    {"xiaomi",      IOT_HUB,    "Xiaomi"},

    // TVs & cast dongles
    {"chromecast",  IOT_TV,     "Chromecast"},
    {"roku",        IOT_TV,     "Roku"},
    {"firetv",      IOT_TV,     "Fire TV"},

    // Printers
    {"hp-print",    IOT_PRINTER, "HP Print"},
    {"hpprint",     IOT_PRINTER, "HP Print"},
    {"brother",     IOT_PRINTER, "Brother"},
    {"canon_ij",    IOT_PRINTER, "Canon"},

    // Consumer routers (identifying default-SSID giveaways)
    {"netgear",     IOT_ROUTER, "Netgear"},
    {"linksys",     IOT_ROUTER, "Linksys"},
    {"tp-link",     IOT_ROUTER, "TP-Link"},
    {"tplink",      IOT_ROUTER, "TP-Link"},
    {"asus",        IOT_ROUTER, "ASUS"},
    {"d-link",      IOT_ROUTER, "D-Link"},
    {"dlink",       IOT_ROUTER, "D-Link"},
    {"xfinitywifi", IOT_ROUTER, "Comcast Xfinity"},
    {"spectrum",    IOT_ROUTER, "Spectrum"},
    {"attwifi",     IOT_ROUTER, "AT&T"},
};
#define SSID_RULE_COUNT ((int)(sizeof(SSID_RULES)/sizeof(SSID_RULES[0])))

struct OuiRule {
    uint8_t oui[3];
    uint8_t cat;
    const char *vendor;
};

// Partial BSSID OUI list for popular IoT device vendors.
static const OuiRule OUI_RULES[] PROGMEM = {
    // Hikvision (BR/EDR + WiFi) — many OUIs; a handful of common prefixes
    {{0xBC, 0xAD, 0x28}, IOT_CAMERA, "Hikvision"},
    {{0x44, 0x19, 0xB6}, IOT_CAMERA, "Hikvision"},
    {{0xC0, 0x51, 0x7E}, IOT_CAMERA, "Hikvision"},
    // Dahua
    {{0x4C, 0x11, 0xBF}, IOT_CAMERA, "Dahua"},
    {{0x3C, 0xEF, 0x8C}, IOT_CAMERA, "Dahua"},
    // Reolink
    {{0xEC, 0x71, 0xDB}, IOT_CAMERA, "Reolink"},
    // Wyze
    {{0x2C, 0xAA, 0x8E}, IOT_CAMERA, "Wyze"},
    {{0x7C, 0x78, 0xB2}, IOT_CAMERA, "Wyze"},
    // Ring (Amazon)
    {{0x8C, 0x85, 0x90}, IOT_HUB,    "Ring"},
    {{0xB0, 0x09, 0xDA}, IOT_HUB,    "Ring"},
    // Google Nest
    {{0xF4, 0xF5, 0xD8}, IOT_HUB,    "Google Nest"},
    {{0xDC, 0xA6, 0x32}, IOT_HUB,    "Google Nest"},
    {{0x48, 0xD6, 0xD5}, IOT_HUB,    "Google Nest"},
    // Amazon Echo
    {{0xFC, 0xA6, 0x67}, IOT_HUB,    "Amazon Echo"},
    {{0x44, 0x65, 0x0D}, IOT_HUB,    "Amazon Echo"},
    {{0x74, 0xC2, 0x46}, IOT_HUB,    "Amazon Echo"},
    // Philips Hue bridge
    {{0x00, 0x17, 0x88}, IOT_HUB,    "Philips Hue"},
    // Sonos
    {{0x5C, 0xAA, 0xFD}, IOT_HUB,    "Sonos"},
    {{0xB8, 0xE9, 0x37}, IOT_HUB,    "Sonos"},
    // Belkin Wemo
    {{0x94, 0x10, 0x3E}, IOT_HUB,    "Belkin Wemo"},
    // TP-Link Kasa
    {{0x50, 0xC7, 0xBF}, IOT_HUB,    "TP-Link Kasa"},
    {{0xB0, 0xBE, 0x76}, IOT_HUB,    "TP-Link"},
    // D-Link
    {{0x1C, 0xAF, 0xF7}, IOT_ROUTER, "D-Link"},
    // Netgear
    {{0xA0, 0x40, 0xA0}, IOT_ROUTER, "Netgear"},
    // ASUS
    {{0xAC, 0x9E, 0x17}, IOT_ROUTER, "ASUS"},
    // Linksys
    {{0x58, 0xEF, 0x68}, IOT_ROUTER, "Linksys"},
    // Chromecast
    {{0xF4, 0xF5, 0xE8}, IOT_TV,     "Chromecast"},
    // Roku
    {{0xB0, 0xA7, 0x37}, IOT_TV,     "Roku"},
    {{0xB8, 0xA1, 0x75}, IOT_TV,     "Roku"},
    // Samsung smart devices
    {{0x5C, 0x49, 0x7D}, IOT_HUB,    "Samsung"},
};
#define OUI_RULE_COUNT ((int)(sizeof(OUI_RULES)/sizeof(OUI_RULES[0])))

static const char *categoryTag(uint8_t cat) {
    switch (cat) {
        case IOT_CAMERA:  return "CAM";
        case IOT_HUB:     return "HUB";
        case IOT_PRINTER: return "PRN";
        case IOT_TV:      return "TV";
        case IOT_ROUTER:  return "RTR";
        default:          return "???";
    }
}

static bool nameContains(const char *hay, const char *needle) {
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

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

struct IotDevice {
    uint8_t  bssid[6];
    char     ssid[33];
    char     vendor[20];
    uint8_t  category;
    int8_t   rssi;
    uint8_t  channel;
    uint32_t lastSeen;
    uint8_t  seenCount;
};

static IotDevice s_devices[IOT_MAX_DEVICES];
static int       s_deviceCount = 0;

static char s_resultBuf[IOT_MAX_DEVICES][IOT_RESULT_LINE_LEN];
static const char *s_resultPtrs[IOT_MAX_DEVICES];
static int  s_resultCount = 0;

static volatile int s_camCount = 0;
static volatile int s_hubCount = 0;

static TaskHandle_t      s_workerHandle = NULL;
static QueueHandle_t     s_cmdQueue     = NULL;
static SemaphoreHandle_t s_doneSem      = NULL;

// ═══════════════════════════════════════════════════════════════════════════
// CLASSIFIER
// ═══════════════════════════════════════════════════════════════════════════

// Returns true if classified (fills cat/vendor); false if no match.
static bool classify(const char *ssid, const uint8_t *bssid,
                     uint8_t *catOut, char *vendorOut, int vendorMax) {
    // 1. SSID substring (case-insensitive)
    for (int i = 0; i < SSID_RULE_COUNT; i++) {
        if (nameContains(ssid, SSID_RULES[i].needle)) {
            *catOut = SSID_RULES[i].cat;
            strncpy(vendorOut, SSID_RULES[i].vendor, vendorMax - 1);
            vendorOut[vendorMax - 1] = '\0';
            return true;
        }
    }
    // 2. BSSID OUI
    for (int i = 0; i < OUI_RULE_COUNT; i++) {
        OuiRule r;
        memcpy_P(&r, &OUI_RULES[i], sizeof(r));
        if (bssid[0] == r.oui[0] && bssid[1] == r.oui[1] && bssid[2] == r.oui[2]) {
            *catOut = r.cat;
            strncpy(vendorOut, r.vendor, vendorMax - 1);
            vendorOut[vendorMax - 1] = '\0';
            return true;
        }
    }
    return false;
}

static int findDeviceByBssid(const uint8_t *bssid) {
    for (int i = 0; i < s_deviceCount; i++) {
        if (memcmp(s_devices[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

static void pruneStale() {
    uint32_t now = millis();
    int i = 0;
    while (i < s_deviceCount) {
        if (now - s_devices[i].lastSeen > IOT_STALE_TIMEOUT_MS) {
            for (int j = i; j < s_deviceCount - 1; j++) s_devices[j] = s_devices[j+1];
            s_deviceCount--;
        } else {
            i++;
        }
    }
}

static void processScan(int n) {
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        const uint8_t *bssid = WiFi.BSSID(i);
        if (!bssid) continue;

        uint8_t cat;
        char vendor[20];
        if (!classify(ssid.c_str(), bssid, &cat, vendor, sizeof(vendor))) continue;

        int idx = findDeviceByBssid(bssid);
        if (idx >= 0) {
            IotDevice *d = &s_devices[idx];
            d->rssi = (int8_t)WiFi.RSSI(i);
            d->channel = (uint8_t)WiFi.channel(i);
            d->lastSeen = millis();
            if (d->seenCount < 255) d->seenCount++;
        } else if (s_deviceCount < IOT_MAX_DEVICES) {
            IotDevice *d = &s_devices[s_deviceCount++];
            memset(d, 0, sizeof(*d));
            memcpy(d->bssid, bssid, 6);
            strncpy(d->ssid, ssid.c_str(), sizeof(d->ssid) - 1);
            strncpy(d->vendor, vendor, sizeof(d->vendor) - 1);
            d->category = cat;
            d->rssi = (int8_t)WiFi.RSSI(i);
            d->channel = (uint8_t)WiFi.channel(i);
            d->lastSeen = millis();
            d->seenCount = 1;
        }
    }
    pruneStale();
}

static void formatResults() {
    int cam = 0, hub = 0;
    int n = 0;
    for (int i = 0; i < s_deviceCount && n < IOT_MAX_DEVICES; i++) {
        IotDevice *d = &s_devices[i];
        if (d->category == IOT_CAMERA) cam++;
        else if (d->category == IOT_HUB) hub++;

        // "CAM Hikvision Camera-1 -42dBm"
        char ssidShort[20];
        strncpy(ssidShort, d->ssid, sizeof(ssidShort) - 1);
        ssidShort[sizeof(ssidShort) - 1] = '\0';
        snprintf(s_resultBuf[n], IOT_RESULT_LINE_LEN, "%s %s %s %ddBm",
                 categoryTag(d->category), d->vendor, ssidShort, (int)d->rssi);
        s_resultPtrs[n] = s_resultBuf[n];
        n++;
    }
    s_camCount = cam;
    s_hubCount = hub;
    if (n == 0) {
        snprintf(s_resultBuf[0], IOT_RESULT_LINE_LEN, "(scanning... no IoT yet)");
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

static void iotReconWorkerTask(void *param) {
    (void)param;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Initial render so the orchestrator can flip IDLE->RUNNING.
    s_deviceCount = 0;
    s_resultCount = 0;
    s_camCount = 0;
    s_hubCount = 0;
    formatResults();
    postRefresh();

    uint32_t lastScanKick = 0;
    bool scanInFlight = false;

    for (;;) {
        uint32_t now = millis();

        if (!scanInFlight && (lastScanKick == 0 || now - lastScanKick >= IOT_RESCAN_INTERVAL_MS)) {
            int rc = WiFi.scanNetworks(true /* async */);
            if (rc == WIFI_SCAN_RUNNING || rc >= 0) {
                scanInFlight = true;
                lastScanKick = now;
            }
        }

        if (scanInFlight) {
            int sc = WiFi.scanComplete();
            if (sc != WIFI_SCAN_RUNNING) {
                if (sc >= 0) processScan(sc);
                WiFi.scanDelete();
                scanInFlight = false;
                postRefresh();
            }
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (cmd.type == CMD_STOP) break;
        }
    }

    if (scanInFlight) {
        esp_wifi_scan_stop();
        WiFi.scanDelete();
    }
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void iotReconStart() {
    if (s_cmdQueue) return;
    if (!radioAcquire(RADIO_WIFI)) return;

    s_resultCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem  = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(iotReconWorkerTask, "iot_recon", 6144,
                            NULL, 2, &s_workerHandle, 0);
}

void iotReconStop() {
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

const char *const *iotReconGetResultLines() { return s_resultPtrs; }
int iotReconGetResultCount() { return s_resultCount; }
int iotReconGetDeviceCount() { return s_deviceCount; }
int iotReconGetCamCount()    { return s_camCount; }
int iotReconGetSmartHubCount() { return s_hubCount; }
