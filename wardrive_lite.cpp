// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Wardrive Lite — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 6)
// ═══════════════════════════════════════════════════════════════════════════

#include "wardrive_lite.h"
#include <WiFi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "storage_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;
static uint32_t s_loggedCount = 0;
static uint32_t s_duplicateCount = 0;

// Same dedup approach as wardriving.cpp: a linear-scan seen-BSSID list
// (WARDRIVING_MAX_NETWORKS=500 there) so re-scanning the same network
// every ~few seconds doesn't spam the CSV with a new row each sweep.
#define WARDRIVE_MAX_SEEN 500
static uint8_t s_seenBSSIDs[WARDRIVE_MAX_SEEN][6];
static int s_seenCount = 0;

static bool isBSSIDSeen(const uint8_t *bssid) {
    for (int i = 0; i < s_seenCount; i++) {
        if (memcmp(s_seenBSSIDs[i], bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void addSeenBSSID(const uint8_t *bssid) {
    if (s_seenCount < WARDRIVE_MAX_SEEN) {
        memcpy(s_seenBSSIDs[s_seenCount], bssid, 6);
        s_seenCount++;
    }
}

static const char *authModeToString(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "[OPEN]";
        case WIFI_AUTH_WEP:             return "[WEP]";
        case WIFI_AUTH_WPA_PSK:         return "[WPA_PSK]";
        case WIFI_AUTH_WPA2_PSK:        return "[WPA2_PSK]";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA_WPA2_PSK]";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2_ENTERPRISE]";
        case WIFI_AUTH_WPA3_PSK:        return "[WPA3_PSK]";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2_WPA3_PSK]";
        default:                        return "[UNKNOWN]";
    }
}

// WigleWifi-1.6 row — same column layout as wardriving.cpp, but lat/lon/
// altitude/accuracy are always 0.0/0.0/0.0/0 (see wardrive_lite.h: no GPS
// on this board).
static void logResult(int i) {
    uint8_t *bssid = WiFi.BSSID(i);

    if (isBSSIDSeen(bssid)) {
        s_duplicateCount++;
        return;
    }

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);

    String ssid = WiFi.SSID(i);
    ssid.replace("\"", "\"\"");
    String ssidField = ssid;
    if (ssid.indexOf(',') >= 0 || ssid.indexOf('"') >= 0) {
        ssidField = "\"" + ssid + "\"";
    }

    int channel = WiFi.channel(i);
    char line[192];
    snprintf(line, sizeof(line),
             "%s,%s,%s,0000-00-00 00:00:00,%d,%d,%d,0.0,0.0,0.0,0,,,WIFI",
             macStr, ssidField.c_str(), authModeToString(WiFi.encryptionType(i)),
             channel, 2407 + channel * 5, WiFi.RSSI(i));

    if (storageLogLine(STORAGE_FILE_WARDRIVE_LOG, line)) {
        s_loggedCount++;
        addSeenBSSID(bssid);
    }
}

static void wardriveLiteWorkerTask(void *param) {
    (void)param;

#ifdef BOARD_M5STICK_S3
    // The StickS3 Loot viewer browses this directory on LittleFS.
    storageEnsureDir("/wardriving");
#endif
    char header[256];
#ifdef BOARD_M5STICK_S3
    snprintf(header, sizeof(header),
             "WigleWifi-1.6,appRelease=" FW_DEVICE ",model=StickS3,release=%s,"
             "device=HaleHound,display=ST7789,board=ESP32-S3,brand=M5Stack,star=Sol,body=3,subBody=0",
             FW_EDITION);
#else
    snprintf(header, sizeof(header),
             "WigleWifi-1.6,appRelease=" FW_DEVICE ",model=T-Embed-CC1101-Plus,release=%s,"
             "device=HaleHound,display=ST7789,board=ESP32-S3,brand=3ntrixx,star=Sol,body=3,subBody=0",
             FW_EDITION);
#endif
    storageLogLine(STORAGE_FILE_WARDRIVE_LOG, header);
    storageLogLine(STORAGE_FILE_WARDRIVE_LOG,
                   "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
                   "CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    WiFi.scanNetworks(true /* async */);

    // Back-to-back async sweeps, no artificial throttle between them — a
    // wardrive wants networks logged as fast as the radio can find them,
    // not paced for a UI's sake. (First version of this file added a ~3s
    // gap between sweeps "to be nice"; that's what made it visibly slower
    // than the original wardriving.cpp's own scan cadence.) Polling
    // WiFi.scanComplete() rather than a blocking scanNetworks(false) call
    // also means an early STOP is honored within ~50ms instead of having
    // to wait out a whole in-progress scan.
    bool stopRequested = false;
    while (!stopRequested) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            for (int i = 0; i < n; i++) {
                logResult(i);
            }
            WiFi.scanDelete();

            RenderMsg msg;
            msg.kind = RENDER_MSG_STATUS_TEXT;
            snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "Logged:%lu  Dupes:%lu",
                     (unsigned long)s_loggedCount, (unsigned long)s_duplicateCount);
            messagingPostTelemetry(msg);

            WiFi.scanNetworks(true);
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && cmd.type == CMD_STOP) {
            stopRequested = true;
        }
    }
    WiFi.scanDelete();

    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void wardriveLiteStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[WARDRIVE_LITE] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_loggedCount = 0;
    s_duplicateCount = 0;
    s_seenCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(wardriveLiteWorkerTask, "wardrive_lite", 6144, NULL, 2, &s_workerHandle, 0);
}

void wardriveLiteStop() {
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
