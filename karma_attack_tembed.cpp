// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Karma Attack — T-Embed CC1101 implementation
// Created: 2026-10-03  (see karma_attack_tembed.h for the port writeup)
// ═══════════════════════════════════════════════════════════════════════════

#include "karma_attack_tembed.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════════════════
// CONSTANTS  (ported verbatim from karma_attack.cpp)
// ═══════════════════════════════════════════════════════════════════════════

#define KA_MAX_SSIDS         32
#define KA_MAX_SSID_LEN      33
#define KA_MAX_CLIENTS       64
#define KA_RESULT_LINE_LEN   40
#define KA_HOP_INTERVAL_MS  500   // matches CYD's KA_CHANNEL_HOP_MS
#define KA_REFRESH_MIN_MS   400

static const uint8_t s_hopChannels[] = { 1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13 };
static const int s_hopChannelCount = (int)(sizeof(s_hopChannels) / sizeof(s_hopChannels[0]));

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t      s_workerHandle = NULL;
static QueueHandle_t     s_cmdQueue     = NULL;
static SemaphoreHandle_t s_doneSem      = NULL;

// Callback -> worker SPSC ring (same shape as probe_sniffer.cpp).
#define KA_RING_SIZE 8    // power of two
struct KarmaRingEntry {
    uint8_t mac[6];
    char    ssid[KA_MAX_SSID_LEN];
};
static KarmaRingEntry s_ring[KA_RING_SIZE];
static volatile uint8_t  s_ringHead = 0;
static volatile uint8_t  s_ringTail = 0;
static volatile uint32_t s_ringDropped = 0;

// Dedup state (worker only — no ISR touches these).
struct SsidEntry {
    char     ssid[KA_MAX_SSID_LEN];
    uint8_t  lastClient[6];
    uint32_t probeCount;
};
static SsidEntry s_ssids[KA_MAX_SSIDS];
static int       s_ssidCount = 0;

static uint8_t s_knownClients[KA_MAX_CLIENTS][6];
static int     s_clientCount = 0;

static volatile uint32_t s_totalProbes = 0;

static char s_lineBuf[KA_MAX_SSIDS][KA_RESULT_LINE_LEN];
static const char *s_linePtrs[KA_MAX_SSIDS];
static int  s_lineCount = 0;

// ═══════════════════════════════════════════════════════════════════════════
// PROMISCUOUS CALLBACK  (ISR context — never touches dedup state)
// ═══════════════════════════════════════════════════════════════════════════

static void IRAM_ATTR karmaRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    int frameLen = pkt->rx_ctrl.sig_len;
    if (frameLen < 24) return;

    // Probe Request = type 0, subtype 4 → first byte 0x40 (with the
    // version bits cleared, same mask probe_sniffer uses).
    uint8_t frameType = payload[0] & 0xFC;
    if (frameType != 0x40) return;

    uint8_t next = (s_ringHead + 1) & (KA_RING_SIZE - 1);
    if (next == s_ringTail) {
        s_ringDropped++;
        return;
    }

    KarmaRingEntry *e = &s_ring[s_ringHead];
    memcpy(e->mac, payload + 10, 6);  // Address 2 = SA

    e->ssid[0] = '\0';
    int pos = 24;
    while (pos + 2 <= frameLen) {
        uint8_t tagNum = payload[pos];
        uint8_t tagLen = payload[pos + 1];
        if (pos + 2 + tagLen > frameLen) break;

        if (tagNum == 0) {  // SSID tag
            if (tagLen > 0 && tagLen < KA_MAX_SSID_LEN) {
                bool valid = true;
                for (int i = 0; i < tagLen; i++) {
                    uint8_t c = payload[pos + 2 + i];
                    if (c < 0x20 || c > 0x7E) { valid = false; break; }
                }
                if (valid) {
                    memcpy(e->ssid, payload + pos + 2, tagLen);
                    e->ssid[tagLen] = '\0';
                }
            }
            break;
        }
        pos += 2 + tagLen;
    }

    s_ringHead = next;
}

// ═══════════════════════════════════════════════════════════════════════════
// DEDUP (worker-task only)
// ═══════════════════════════════════════════════════════════════════════════

static bool isClientKnown(const uint8_t *mac) {
    for (int i = 0; i < s_clientCount; i++) {
        if (memcmp(s_knownClients[i], mac, 6) == 0) return true;
    }
    return false;
}

static void addClient(const uint8_t *mac) {
    if (s_clientCount < KA_MAX_CLIENTS && !isClientKnown(mac)) {
        memcpy(s_knownClients[s_clientCount], mac, 6);
        s_clientCount++;
    }
}

static int findSsid(const char *ssid) {
    for (int i = 0; i < s_ssidCount; i++) {
        if (strcasecmp(s_ssids[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

static void recordSsid(const char *ssid, const uint8_t *clientMac) {
    if (ssid[0] == '\0') return;  // broadcast probe — skip, matches CYD
    int idx = findSsid(ssid);
    if (idx >= 0) {
        s_ssids[idx].probeCount++;
        memcpy(s_ssids[idx].lastClient, clientMac, 6);
    } else if (s_ssidCount < KA_MAX_SSIDS) {
        strncpy(s_ssids[s_ssidCount].ssid, ssid, KA_MAX_SSID_LEN - 1);
        s_ssids[s_ssidCount].ssid[KA_MAX_SSID_LEN - 1] = '\0';
        memcpy(s_ssids[s_ssidCount].lastClient, clientMac, 6);
        s_ssids[s_ssidCount].probeCount = 1;
        s_ssidCount++;
    }
}

static void formatLines() {
    for (int i = 0; i < s_ssidCount; i++) {
        SsidEntry &e = s_ssids[i];
        // "<ssid> (<cnt>) <mac3>" — the last 3 MAC bytes of the latest
        // client that probed for this SSID, same density as the CYD view.
        snprintf(s_lineBuf[i], KA_RESULT_LINE_LEN, "%s (%lu) %02X%02X%02X",
                 e.ssid, (unsigned long)e.probeCount,
                 e.lastClient[3], e.lastClient[4], e.lastClient[5]);
        s_linePtrs[i] = s_lineBuf[i];
    }
    if (s_ssidCount == 0) {
        snprintf(s_lineBuf[0], KA_RESULT_LINE_LEN, "(listening for probes...)");
        s_linePtrs[0] = s_lineBuf[0];
        s_lineCount = 0;
    } else {
        s_lineCount = s_ssidCount;
    }
}

static void postRefresh() {
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE;   // repurposed as "list updated"
    messagingPostTelemetry(msg);
}

// ═══════════════════════════════════════════════════════════════════════════
// WIFI PROMISCUOUS INIT / CLEANUP (ported from probe_sniffer.cpp)
// ═══════════════════════════════════════════════════════════════════════════

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
        Serial.println("[KARMA] promiscuous init FAILED after 3 attempts");
        #endif
        return false;
    }

    wifi_promiscuous_filter_t filter;
    memset(&filter, 0, sizeof(filter));
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&karmaRxCb);
    return true;
}

static void promiscuousCleanup() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    esp_wifi_deinit();
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER TASK
// ═══════════════════════════════════════════════════════════════════════════

static void karmaWorkerTask(void *param) {
    (void)param;

    s_ssidCount    = 0;
    s_clientCount  = 0;
    s_totalProbes  = 0;
    s_lineCount    = 0;
    s_ringHead     = 0;
    s_ringTail     = 0;
    s_ringDropped  = 0;

    formatLines();   // publish the "(listening for probes...)" placeholder promptly
    postRefresh();

    if (!promiscuousInit()) {
        radioRelease(RADIO_WIFI);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    int currentHop = 0;
    esp_wifi_set_channel(s_hopChannels[currentHop], WIFI_SECOND_CHAN_NONE);

    uint32_t lastHop     = millis();
    uint32_t lastRefresh = 0;
    bool     dirty       = false;

    for (;;) {
        // Drain the ring first — hot path.
        while (s_ringTail != s_ringHead) {
            KarmaRingEntry e;
            memcpy(&e, (const void *)&s_ring[s_ringTail], sizeof(e));
            s_ringTail = (s_ringTail + 1) & (KA_RING_SIZE - 1);

            s_totalProbes++;
            addClient(e.mac);
            recordSsid(e.ssid, e.mac);
            dirty = true;
        }

        uint32_t now = millis();
        if (now - lastHop >= KA_HOP_INTERVAL_MS) {
            currentHop = (currentHop + 1) % s_hopChannelCount;
            esp_wifi_set_channel(s_hopChannels[currentHop], WIFI_SECOND_CHAN_NONE);
            lastHop = now;
        }

        if (dirty && (now - lastRefresh >= KA_REFRESH_MIN_MS)) {
            dirty = false;
            lastRefresh = now;
            formatLines();
            postRefresh();
        }

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

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void karmaStart() {
    if (!radioAcquire(RADIO_WIFI)) return;

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem  = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(karmaWorkerTask, "karma", 4096,
                            NULL, 2, &s_workerHandle, 0);
}

void karmaStop() {
    if (!s_cmdQueue) return;

    Cmd cmd;
    cmd.type = CMD_STOP;
    xQueueSend(s_cmdQueue, &cmd, portMAX_DELAY);
    xSemaphoreTake(s_doneSem, portMAX_DELAY);

    vQueueDelete(s_cmdQueue);
    vSemaphoreDelete(s_doneSem);
    s_cmdQueue     = NULL;
    s_doneSem      = NULL;
    s_workerHandle = NULL;
}

const char *const *karmaGetResultLines() {
    return s_linePtrs;
}

int karmaGetResultCount() {
    // Mirror probe_sniffer's "(no probes yet)" placeholder trick: when
    // the real pool is empty, return 1 so the UI shows a single text row.
    return (s_lineCount > 0) ? s_lineCount : 1;
}

const char *karmaGetSsid(int index) {
    if (index < 0 || index >= s_ssidCount) return NULL;
    return s_ssids[index].ssid;
}

void karmaFormatStatus(char *buf, int bufLen) {
    if (!buf || bufLen <= 0) return;
    snprintf(buf, bufLen, "P:%lu S:%d C:%d",
             (unsigned long)s_totalProbes, s_ssidCount, s_clientCount);
}
