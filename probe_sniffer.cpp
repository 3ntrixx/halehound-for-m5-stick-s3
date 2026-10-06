// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Probe Sniffer — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "probe_sniffer.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define PROBE_MAX_LOG           16   // matches DeauthDetect's MAX_LINES
#define PROBE_MAX_DEVICES       30   // matches DeauthDetect's MAX_DEVICES
#define PROBE_MAX_SSIDS         50   // matches DeauthDetect's MAX_SSIDS
#define PROBE_RESULT_LINE_LEN   40
#define PROBE_HOP_INTERVAL_MS  300   // matches the original's channel-hop cadence
#define PROBE_REFRESH_MIN_MS   500

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Promiscuous callback -> 8-slot SPSC ring buffer, drained by the worker.
// The single-slot mailbox this replaces dropped every probe that arrived
// while the worker was blocked in its 50 ms command-queue wait (~20
// drains/sec max); the worker now drains the whole ring every iteration and
// blocks at most 5 ms.
#define PROBE_RING_SIZE 8   // power of two
struct ProbeRingEntry {
    uint8_t mac[6];
    char ssid[33];
};
static ProbeRingEntry s_ring[PROBE_RING_SIZE];
static volatile uint8_t s_ringHead = 0;   // producer: callback
static volatile uint8_t s_ringTail = 0;   // consumer: worker
static volatile uint32_t s_ringDropped = 0;

static void IRAM_ATTR probeRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;

    uint8_t frameType = payload[0] & 0xFC;
    if (frameType != 0x40) return; // 0x40 = Probe Request

    uint8_t next = (s_ringHead + 1) & (PROBE_RING_SIZE - 1);
    if (next == s_ringTail) {
        s_ringDropped++;
        return;
    }

    ProbeRingEntry *e = &s_ring[s_ringHead];
    memcpy(e->mac, payload + 10, 6);

    int pos = 24;
    int frameLen = pkt->rx_ctrl.sig_len;
    e->ssid[0] = '\0';
    while (pos < frameLen - 2) {
        uint8_t tagNum = payload[pos];
        uint8_t tagLen = payload[pos + 1];
        if (tagNum == 0) { // SSID tag
            if (tagLen > 0 && tagLen <= 32) {
                memcpy(e->ssid, payload + pos + 2, tagLen);
                e->ssid[tagLen] = '\0';
            }
            break;
        }
        pos += 2 + tagLen;
    }
    if (e->ssid[0] == '\0') {
        strcpy(e->ssid, "[BROADCAST]");
    }

    s_ringHead = next;
}

// ── Dedup + scrolling log (worker task only — no ISR touches these) ─────
static uint8_t s_knownDevices[PROBE_MAX_DEVICES][6];
static int s_deviceCount = 0;
static char s_knownSsids[PROBE_MAX_SSIDS][33];
static int s_ssidCount = 0;

static char s_logBuf[PROBE_MAX_LOG][PROBE_RESULT_LINE_LEN];
static const char *s_logPtrs[PROBE_MAX_LOG];
static int s_logCount = 0;

static bool isDeviceKnown(const uint8_t *mac) {
    for (int i = 0; i < s_deviceCount; i++) {
        if (memcmp(mac, s_knownDevices[i], 6) == 0) return true;
    }
    return false;
}

static void addDevice(const uint8_t *mac) {
    if (s_deviceCount < PROBE_MAX_DEVICES && !isDeviceKnown(mac)) {
        memcpy(s_knownDevices[s_deviceCount], mac, 6);
        s_deviceCount++;
    }
}

static bool isSsidKnown(const char *ssid) {
    for (int i = 0; i < s_ssidCount; i++) {
        if (strcmp(s_knownSsids[i], ssid) == 0) return true;
    }
    return false;
}

static void addSsid(const char *ssid) {
    if (s_ssidCount < PROBE_MAX_SSIDS && !isSsidKnown(ssid) && ssid[0] != '\0') {
        strncpy(s_knownSsids[s_ssidCount], ssid, 32);
        s_knownSsids[s_ssidCount][32] = '\0';
        s_ssidCount++;
    }
}

// Scroll-if-full, matches DeauthDetect::addProbeToLog() (newest at the
// bottom, oldest falls off the top).
static void addLogEntry(const uint8_t *mac, const char *ssid, bool isNew) {
    if (s_logCount >= PROBE_MAX_LOG) {
        for (int i = 0; i < PROBE_MAX_LOG - 1; i++) {
            memcpy(s_logBuf[i], s_logBuf[i + 1], PROBE_RESULT_LINE_LEN);
        }
        s_logCount = PROBE_MAX_LOG - 1;
    }
    snprintf(s_logBuf[s_logCount], PROBE_RESULT_LINE_LEN, "%c %02X:%02X:%02X %s",
             isNew ? '*' : ' ', mac[3], mac[4], mac[5], ssid);
    s_logCount++;
    for (int i = 0; i < s_logCount; i++) s_logPtrs[i] = s_logBuf[i];
}

static void postRefresh() {
    if (s_logCount == 0) {
        snprintf(s_logBuf[0], PROBE_RESULT_LINE_LEN, "(no probes yet)");
        s_logPtrs[0] = s_logBuf[0];
    }
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE; // repurposed as "list updated", matches ble_scan.cpp
    messagingPostTelemetry(msg);
}

// Same promiscuous init/cleanup shape as packet_monitor.cpp/wifi_guardian.cpp.
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
        Serial.println("[PROBE_SNIFFER] promiscuous init FAILED after 3 attempts");
        #endif
        return false;
    }

    // Only management frames can carry probe requests — filtering here keeps
    // the callback (and the ring) free of data-frame traffic.
    wifi_promiscuous_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&probeRxCb);
    return true;
}

static void promiscuousCleanup() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    esp_wifi_deinit();
}

static void probeSniffWorkerTask(void *param) {
    (void)param;

    s_deviceCount = 0;
    s_ssidCount = 0;
    s_logCount = 0;
    s_ringHead = 0;
    s_ringTail = 0;
    s_ringDropped = 0;

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
    bool dirty = true; // post the initial "(no probes yet)" placeholder promptly

    for (;;) {
        // Drain every captured probe first — this is the hot path. Copy the
        // entry out before advancing the tail so the producer can safely
        // reuse the slot.
        bool processed = false;
        while (s_ringTail != s_ringHead) {
            ProbeRingEntry e;
            memcpy(&e, (const void *)&s_ring[s_ringTail], sizeof(e));
            s_ringTail = (s_ringTail + 1) & (PROBE_RING_SIZE - 1);

            bool isNew = !isDeviceKnown(e.mac);
            addDevice(e.mac);
            addSsid(e.ssid);
            addLogEntry(e.mac, e.ssid, isNew);
            processed = true;
        }
        if (processed) {
            dirty = true;
        }

        uint32_t now = millis();
        if (now - lastHop >= PROBE_HOP_INTERVAL_MS) {
            currentChannel = (currentChannel % 13) + 1;
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            lastHop = now;
        }

        if (dirty && (now - lastRefresh >= PROBE_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            postRefresh();
        }

        // Short wait so the ring above is drained promptly (the old 50 ms
        // wait capped capture at ~20 probes/sec) while still reacting to
        // CMD_STOP quickly.
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(5)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }
    }

    promiscuousCleanup();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void probeSniffStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[PROBE_SNIFFER] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(probeSniffWorkerTask, "probe_sniff", 4096, NULL, 2, &s_workerHandle, 0);
}

void probeSniffStop() {
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

const char *const *probeSniffGetResultLines() {
    return s_logPtrs;
}

int probeSniffGetResultCount() {
    return (s_logCount > 0) ? s_logCount : 1; // the "(no probes yet)" placeholder row
}
