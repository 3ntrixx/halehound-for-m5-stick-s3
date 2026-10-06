// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Guardian — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "wifi_guardian.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// Constants ported verbatim from jam_detect.cpp's WiFiGuardian namespace.
#define WG_CAL_DURATION_MS            5000
#define WG_HOP_INTERVAL_MS             500
#define WG_DEAUTH_SUSPICIOUS_DELTA        5
#define WG_DEAUTH_JAMMING_DELTA          20
#define WG_BEACON_SUSPICIOUS_MULT         3
#define WG_BEACON_JAMMING_MULT           10
#define WG_THREAT_CLEAR_TIMEOUT_MS     3000

static const uint8_t HOP_CHANNELS[3] = {1, 6, 11};

// Same shape as subghz_sentinel.cpp's ThreatLevel — duplicated per this
// project's own precedent (see wifi_guardian.h).
enum ThreatLevel { THREAT_CALIBRATING, THREAT_CLEAR, THREAT_SUSPICIOUS, THREAT_JAMMING };

static const char *threatText(ThreatLevel level) {
    switch (level) {
        case THREAT_CALIBRATING: return "CALIBRATING";
        case THREAT_CLEAR:       return "CLEAR";
        case THREAT_SUSPICIOUS:  return "SUSPICIOUS";
        case THREAT_JAMMING:     return "JAMMED!";
        default:                 return "UNKNOWN";
    }
}

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// Promiscuous counters — updated from the RX callback, matching
// jam_detect.cpp's wifiPromiscCB() (same frame-control subtype bytes
// packet_monitor.cpp already checks for its own deauth counter: 0xA0
// disassoc, 0xC0 deauth — plus 0x80 beacon here for the flood check).
static volatile uint32_t s_deauthCount = 0;
static volatile uint32_t s_disassocCount = 0;
static volatile uint32_t s_beaconCount = 0;
static volatile int32_t s_lastRssi = 0;

static void IRAM_ATTR promiscRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    s_lastRssi = pkt->rx_ctrl.rssi;

    uint8_t frameType = pkt->payload[0];
    if (frameType == 0xA0) s_deauthCount++;
    else if (frameType == 0xC0) s_disassocCount++;
    else if (frameType == 0x80) s_beaconCount++;
}

// Same promiscuous init/cleanup shape as packet_monitor.cpp (retry loop,
// WIFI_STORAGE_RAM, mode NULL then start).
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
        Serial.println("[WIFI_GUARDIAN] promiscuous init FAILED after 3 attempts");
        #endif
        return false;
    }

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&promiscRxCb);
    return true;
}

static void promiscuousCleanup() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    esp_wifi_deinit();
}

// Ports WiFiGuardian::loop()'s rate-calc/calibration/classification block
// verbatim (see wifi_guardian.h). No separate radio/dsp task split here
// (unlike subghz_sentinel.cpp) — sampling is the promiscuous callback, not
// an active per-frame CC1101 sweep, so there's nothing for a second task to
// protect against being stalled by.
static void guardianWorkerTask(void *param) {
    (void)param;

    if (!promiscuousInit()) {
        radioRelease(RADIO_WIFI);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    uint8_t hopIndex = 0;
    uint8_t currentChannel = HOP_CHANNELS[0];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    s_deauthCount = 0;
    s_disassocCount = 0;
    s_beaconCount = 0;
    uint32_t prevDeauth = 0, prevDisassoc = 0, prevBeacon = 0;
    uint32_t deauthRate = 0, disassocRate = 0, beaconRate = 0;
    uint32_t baseDeauthRate = 0, baseDisassocRate = 0, baseBeaconRate = 0;
    uint32_t calSamples = 0, calDeauthSum = 0, calDisassocSum = 0, calBeaconSum = 0;

    ThreatLevel threat = THREAT_CALIBRATING;
    unsigned long calStartTime = millis();
    unsigned long threatClearTimer = calStartTime;
    unsigned long lastRateCalc = millis();
    unsigned long lastHop = millis();

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }

        unsigned long now = millis();

        if (now - lastHop >= WG_HOP_INTERVAL_MS) {
            hopIndex = (hopIndex + 1) % 3;
            currentChannel = HOP_CHANNELS[hopIndex];
            esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
            lastHop = now;
        }

        if (now - lastRateCalc < 1000) {
            continue;
        }

        deauthRate = s_deauthCount - prevDeauth;
        disassocRate = s_disassocCount - prevDisassoc;
        beaconRate = s_beaconCount - prevBeacon;
        prevDeauth = s_deauthCount;
        prevDisassoc = s_disassocCount;
        prevBeacon = s_beaconCount;
        lastRateCalc = now;

        if (threat == THREAT_CALIBRATING) {
            calDeauthSum += deauthRate;
            calDisassocSum += disassocRate;
            calBeaconSum += beaconRate;
            calSamples++;

            unsigned long elapsed = now - calStartTime;
            if (elapsed >= WG_CAL_DURATION_MS && calSamples > 0) {
                baseDeauthRate = calDeauthSum / calSamples;
                baseDisassocRate = calDisassocSum / calSamples;
                baseBeaconRate = max(calBeaconSum / calSamples, (uint32_t)1);
                threat = THREAT_CLEAR;
                threatClearTimer = now;
            } else {
                RenderMsg msg;
                msg.kind = RENDER_MSG_STATUS_TEXT;
                int pct = constrain((int)((elapsed * 100) / WG_CAL_DURATION_MS), 0, 100);
                snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "Calibrating WiFi... %d%%", pct);
                messagingPostTelemetry(msg);
                continue;
            }
        } else {
            ThreatLevel newThreat = THREAT_CLEAR;

            uint32_t attackRate = deauthRate + disassocRate;
            uint32_t attackBase = baseDeauthRate + baseDisassocRate;
            if (attackRate > attackBase + WG_DEAUTH_JAMMING_DELTA) {
                newThreat = THREAT_JAMMING;
            } else if (attackRate > attackBase + WG_DEAUTH_SUSPICIOUS_DELTA) {
                newThreat = THREAT_SUSPICIOUS;
            }

            if (beaconRate > baseBeaconRate * WG_BEACON_JAMMING_MULT) {
                newThreat = THREAT_JAMMING;
            } else if (beaconRate > baseBeaconRate * WG_BEACON_SUSPICIOUS_MULT && newThreat < THREAT_SUSPICIOUS) {
                newThreat = THREAT_SUSPICIOUS;
            }

            if (newThreat > THREAT_CLEAR) {
                threat = newThreat;
                threatClearTimer = now;
            } else if (threat > THREAT_CLEAR && now - threatClearTimer >= WG_THREAT_CLEAR_TIMEOUT_MS) {
                threat = THREAT_CLEAR;
            }
        }

        RenderMsg msg;
        msg.kind = RENDER_MSG_STATUS_TEXT;
        snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "Ch%d %s D:%lu Ds:%lu B:%lu",
                 currentChannel, threatText(threat),
                 (unsigned long)deauthRate, (unsigned long)disassocRate, (unsigned long)beaconRate);
        messagingPostTelemetry(msg);
    }

    promiscuousCleanup();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void wifiGuardianStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[WIFI_GUARDIAN] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(guardianWorkerTask, "wifi_guardian", 4096, NULL, 2, &s_workerHandle, 0);
}

void wifiGuardianStop() {
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
