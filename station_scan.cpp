// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Station Scanner — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "station_scan.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "oui_registry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SS_MAX_STATIONS      50   // matches StationScan::MAX_STATIONS
#define SS_RESULT_LINE_LEN   64
#define SS_CHANNEL_HOP_MS   300   // matches StationScan::CHANNEL_HOP_MS
#define SS_MAX_CHANNEL       13
#define SS_REFRESH_MIN_MS   500

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Promiscuous callback -> single-slot mailbox, same pattern as
// probe_sniffer.cpp's probeRxCb ────────────────────────────────────────
static volatile bool s_pendingReady = false;
static uint8_t s_pendingMac[6];
static int8_t s_pendingRssi = 0;
static uint8_t s_pendingApMac[6];
static uint8_t s_pendingApChannel = 0;
static volatile bool s_pendingHasAP = false;

// Ported verbatim from StationScan::snifferCallback() — Probe Requests
// (client searching, no AP known) and Data frames (client<->AP, ToDS/
// FromDS bits recover the BSSID, same offsets as the original).
static void IRAM_ATTR stationRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (s_pendingReady) return; // previous frame not yet drained — skip

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 24) return;
    uint8_t *payload = pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;
    uint8_t frameControl = payload[0];
    uint8_t frameType = frameControl & 0x0C;
    uint8_t frameSubtype = frameControl >> 4;

    if (type == WIFI_PKT_MGMT && frameType == 0x00 && frameSubtype == 0x04) {
        uint8_t *srcMac = payload + 10;
        if (srcMac[0] & 0x01) return; // broadcast/multicast

        memcpy((void *)s_pendingMac, srcMac, 6);
        s_pendingRssi = rssi;
        s_pendingHasAP = false;
        s_pendingReady = true;
        return;
    }

    if (type == WIFI_PKT_DATA && frameType == 0x08) {
        uint8_t flags = payload[1];
        bool toDS = flags & 0x01;
        bool fromDS = flags & 0x02;

        uint8_t *clientMac = NULL;
        uint8_t *bssid = NULL;

        if (toDS && !fromDS) {
            bssid = payload + 4;
            clientMac = payload + 10;
        } else if (!toDS && fromDS) {
            clientMac = payload + 4;
            bssid = payload + 10;
        } else {
            return; // WDS or IBSS — skip
        }

        if (clientMac[0] & 0x01) return;

        memcpy((void *)s_pendingMac, clientMac, 6);
        memcpy((void *)s_pendingApMac, bssid, 6);
        s_pendingApChannel = pkt->rx_ctrl.channel;
        s_pendingRssi = rssi;
        s_pendingHasAP = true;
        s_pendingReady = true;
        return;
    }
}

// Ported verbatim from StationScan::lookupVendor() — top-manufacturer OUI
// prefixes, "Random" for locally-administered (iOS/Android privacy) MACs.
static const char *lookupVendor(const uint8_t *mac) {
    if (mac[0] & 0x02) return "Random";

    if (mac[0] == 0x00 && mac[1] == 0x1C && mac[2] == 0xB3) return "Apple";
    if (mac[0] == 0xF0 && mac[1] == 0x18 && mac[2] == 0x98) return "Apple";
    if (mac[0] == 0x3C && mac[1] == 0x06 && mac[2] == 0x30) return "Apple";
    if (mac[0] == 0xA4 && mac[1] == 0x83 && mac[2] == 0xE7) return "Apple";
    if (mac[0] == 0x14 && mac[1] == 0x98 && mac[2] == 0x77) return "Apple";
    if (mac[0] == 0x80 && mac[1] == 0xE6 && mac[2] == 0x50) return "Apple";
    if (mac[0] == 0x00 && mac[1] == 0x26 && mac[2] == 0x37) return "Samsung";
    if (mac[0] == 0xE4 && mac[1] == 0x7C && mac[2] == 0xF9) return "Samsung";
    if (mac[0] == 0x94 && mac[1] == 0x35 && mac[2] == 0x0A) return "Samsung";
    if (mac[0] == 0x8C && mac[1] == 0x71 && mac[2] == 0xF8) return "Samsung";
    if (mac[0] == 0x94 && mac[1] == 0xEB && mac[2] == 0x2C) return "Google";
    if (mac[0] == 0x3C && mac[1] == 0x28 && mac[2] == 0x6D) return "Google";
    if (mac[0] == 0xF4 && mac[1] == 0xF5 && mac[2] == 0xE8) return "Google";
    if (mac[0] == 0x00 && mac[1] == 0x1E && mac[2] == 0x64) return "Intel";
    if (mac[0] == 0x8C && mac[1] == 0x8D && mac[2] == 0x28) return "Intel";
    if (mac[0] == 0x48 && mac[1] == 0x51 && mac[2] == 0xB7) return "Intel";
    if (mac[0] == 0x24 && mac[1] == 0x6F && mac[2] == 0x28) return "Esprsf";
    if (mac[0] == 0xA4 && mac[1] == 0xCF && mac[2] == 0x12) return "Esprsf";
    if (mac[0] == 0x30 && mac[1] == 0xAE && mac[2] == 0xA4) return "Esprsf";
    if (mac[0] == 0xAC && mac[1] == 0x67 && mac[2] == 0xB2) return "Esprsf";
    if (mac[0] == 0xA4 && mac[1] == 0xE5 && mac[2] == 0x7C) return "Esprsf";
    if (mac[0] == 0xB8 && mac[1] == 0x27 && mac[2] == 0xEB) return "RasPi";
    if (mac[0] == 0xDC && mac[1] == 0xA6 && mac[2] == 0x32) return "RasPi";
    if (mac[0] == 0xE4 && mac[1] == 0x5F && mac[2] == 0x01) return "RasPi";
    if (mac[0] == 0x74 && mac[1] == 0xC2 && mac[2] == 0x46) return "Amazon";
    if (mac[0] == 0xF0 && mac[1] == 0x27 && mac[2] == 0x2D) return "Amazon";
    if (mac[0] == 0x44 && mac[1] == 0x65 && mac[2] == 0x0D) return "Amazon";
    if (mac[0] == 0x48 && mac[1] == 0x46 && mac[2] == 0xFB) return "Huawei";
    if (mac[0] == 0x00 && mac[1] == 0x9A && mac[2] == 0xCD) return "Huawei";
    if (mac[0] == 0x94 && mac[1] == 0x65 && mac[2] == 0x2D) return "OnePlus";
    if (mac[0] == 0xC0 && mac[1] == 0xEE && mac[2] == 0xFB) return "OnePlus";
    if (mac[0] == 0x64 && mac[1] == 0xCC && mac[2] == 0x2E) return "Xiaomi";
    if (mac[0] == 0x78 && mac[1] == 0x02 && mac[2] == 0xF8) return "Xiaomi";
    if (mac[0] == 0x50 && mac[1] == 0xC7 && mac[2] == 0xBF) return "TP-Link";
    if (mac[0] == 0xEC && mac[1] == 0x08 && mac[2] == 0x6B) return "TP-Link";
    if (mac[0] == 0x00 && mac[1] == 0xE0 && mac[2] == 0x4C) return "Realtek";
    if (mac[0] == 0x48 && mac[1] == 0x5D && mac[2] == 0x60) return "Realtek";
    if (mac[0] == 0x28 && mac[1] == 0x18 && mac[2] == 0x78) return "Msft";
    if (mac[0] == 0x7C && mac[1] == 0x1E && mac[2] == 0x52) return "Msft";
    if (mac[0] == 0x00 && mac[1] == 0x04 && mac[2] == 0x1F) return "Sony";
    if (mac[0] == 0xFC && mac[1] == 0x0F && mac[2] == 0xE6) return "Sony";
    return "???";
}

// ── Discovered-station tracking (worker task only — no ISR touches these) ──
struct Station {
    uint8_t mac[6];
    int8_t rssi;
    char vendor[21];
    uint8_t apBssid[6];
    uint8_t apChannel;
    bool associated;
};
static Station s_stations[SS_MAX_STATIONS];
static int s_stationCount = 0;

static char s_lineBuf[SS_MAX_STATIONS][SS_RESULT_LINE_LEN];
static const char *s_linePtrs[SS_MAX_STATIONS];

static int findStation(const uint8_t *mac) {
    for (int i = 0; i < s_stationCount; i++) {
        if (memcmp(s_stations[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static void addOrUpdateStation(const uint8_t *mac, int8_t rssi, const uint8_t *apMac, uint8_t apChan, bool hasAP) {
    int idx = findStation(mac);
    if (idx >= 0) {
        s_stations[idx].rssi = rssi;
        if (hasAP) {
            memcpy(s_stations[idx].apBssid, apMac, 6);
            s_stations[idx].apChannel = apChan;
            s_stations[idx].associated = true;
        }
        return;
    }
    if (s_stationCount >= SS_MAX_STATIONS) return;

    Station &s = s_stations[s_stationCount];
    memcpy(s.mac, mac, 6);
    s.rssi = rssi;
    if (!ouiRegistryLookup(mac, s.vendor, sizeof(s.vendor))) {
        snprintf(s.vendor, sizeof(s.vendor), "%s", lookupVendor(mac));
    }
    if (hasAP) {
        memcpy(s.apBssid, apMac, 6);
        s.apChannel = apChan;
        s.associated = true;
    } else {
        memset(s.apBssid, 0, 6);
        s.apChannel = 0;
        s.associated = false;
    }
    s_stationCount++;
}

static void formatResults() {
    for (int i = 0; i < s_stationCount; i++) {
        const Station &s = s_stations[i];
        if (s.associated) {
            snprintf(s_lineBuf[i], SS_RESULT_LINE_LEN, "%02X:%02X:%02X %ddB %.12s AP:%02X:%02X:%02X C%d",
                     s.mac[3], s.mac[4], s.mac[5], s.rssi, s.vendor,
                     s.apBssid[3], s.apBssid[4], s.apBssid[5], s.apChannel);
        } else {
            snprintf(s_lineBuf[i], SS_RESULT_LINE_LEN, "%02X:%02X:%02X %ddB %s",
                     s.mac[3], s.mac[4], s.mac[5], s.rssi, s.vendor);
        }
        s_linePtrs[i] = s_lineBuf[i];
    }
    if (s_stationCount == 0) {
        snprintf(s_lineBuf[0], SS_RESULT_LINE_LEN, "(no stations yet)");
        s_linePtrs[0] = s_lineBuf[0];
    }
}

// Same promiscuous init/cleanup shape as probe_sniffer.cpp.
static bool promiscuousInit() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);

    nvs_flash_init();
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            esp_wifi_deinit();
            delay(100);
            continue;
        }
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        err = esp_wifi_set_mode(WIFI_MODE_NULL);
        if (err != ESP_OK) { esp_wifi_deinit(); delay(100); continue; }
        err = esp_wifi_start();
        if (err != ESP_OK) { esp_wifi_stop(); esp_wifi_deinit(); delay(100); continue; }
        break;
    }
    if (err != ESP_OK) {
        #if CYD_DEBUG
        Serial.println("[STATION_SCAN] promiscuous init FAILED after 3 attempts");
        #endif
        return false;
    }

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&stationRxCb);
    return true;
}

static void promiscuousCleanup() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void stationScanWorkerTask(void *param) {
    (void)param;

    s_stationCount = 0;
    s_pendingReady = false;

    if (!promiscuousInit()) {
        radioRelease(RADIO_WIFI);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    uint8_t currentChannel = 1;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    uint32_t lastHop = millis();
    uint32_t lastRefresh = 0;
    bool dirty = true; // post the initial "(no stations yet)" placeholder promptly

    for (;;) {
        if (s_pendingReady) {
            uint8_t mac[6], apMac[6];
            int8_t rssi;
            uint8_t apChannel;
            bool hasAP;

            memcpy(mac, (const void *)s_pendingMac, 6);
            memcpy(apMac, (const void *)s_pendingApMac, 6);
            rssi = s_pendingRssi;
            apChannel = s_pendingApChannel;
            hasAP = s_pendingHasAP;
            s_pendingReady = false;

            addOrUpdateStation(mac, rssi, apMac, apChannel, hasAP);
            dirty = true;
        }

        uint32_t now = millis();
        if (now - lastHop >= SS_CHANNEL_HOP_MS) {
            currentChannel = (currentChannel % SS_MAX_CHANNEL) + 1;
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            lastHop = now;
        }

        if (dirty && (now - lastRefresh >= SS_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            formatResults();

            RenderMsg msg;
            msg.kind = RENDER_MSG_SCAN_COMPLETE; // repurposed as "list updated", matches probe_sniffer.cpp
            messagingPostTelemetry(msg);
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }
    }

    promiscuousCleanup();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void stationScanStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[STATION_SCAN] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    // Vendor lookups now perform SD filesystem calls on this worker.
    xTaskCreatePinnedToCore(stationScanWorkerTask, "station_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void stationScanStop() {
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

const char *const *stationScanGetResultLines() {
    return s_linePtrs;
}

int stationScanGetResultCount() {
    return (s_stationCount > 0) ? s_stationCount : 1;
}
