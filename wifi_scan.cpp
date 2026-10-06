// ═══════════════════════════════════════════════════════════════════════════
// HaleHound WiFi Scanner — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 4)
// ═══════════════════════════════════════════════════════════════════════════

#include "wifi_scan.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define MAX_RESULTS 20
#define RESULT_LINE_LEN 56

static char s_resultBuf[MAX_RESULTS][RESULT_LINE_LEN];
static const char *s_resultPtrs[MAX_RESULTS];
static int s_resultCount = 0;

// Raw per-result data for the Deauther tap-to-attack handoff (see
// wifi_scan.h) — snapshotted alongside the display strings below rather
// than read live via WiFi.BSSID()/channel() from a different task later,
// matching this project's usual "copy into a static array, don't share
// live driver state across tasks" idiom (eapol_sniff.cpp's APInfo, etc.).
static uint8_t s_resultBssid[MAX_RESULTS][6];
static char s_resultSsid[MAX_RESULTS][33];
static uint8_t s_resultChannel[MAX_RESULTS];

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// Strongest-signal-first, matching wifi_attacks.cpp's WifiScan namespace
// default (SORT_RSSI) — its own comment calls it out as the default sort.
static int s_sortedIndices[MAX_RESULTS];

static void sortByRssi(int n) {
    for (int i = 0; i < n; i++) s_sortedIndices[i] = i;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            if (WiFi.RSSI(s_sortedIndices[j]) > WiFi.RSSI(s_sortedIndices[i])) {
                int tmp = s_sortedIndices[i];
                s_sortedIndices[i] = s_sortedIndices[j];
                s_sortedIndices[j] = tmp;
            }
        }
    }
}

static void formatResults(int n) {
    s_resultCount = (n > MAX_RESULTS) ? MAX_RESULTS : (n < 0 ? 0 : n);
    sortByRssi(s_resultCount);
    for (int row = 0; row < s_resultCount; row++) {
        int i = s_sortedIndices[row];
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) ssid = "(hidden)";
        snprintf(s_resultBuf[row], RESULT_LINE_LEN, "%s %ddBm CH%d",
                 ssid.c_str(), WiFi.RSSI(i), WiFi.channel(i));
        s_resultPtrs[row] = s_resultBuf[row];

        memcpy(s_resultBssid[row], WiFi.BSSID(i), 6);
        strncpy(s_resultSsid[row], ssid.c_str(), 32);
        s_resultSsid[row][32] = '\0';
        s_resultChannel[row] = (uint8_t)WiFi.channel(i);
    }
    if (s_resultCount == 0) {
        snprintf(s_resultBuf[0], RESULT_LINE_LEN, "(no networks found)");
        s_resultPtrs[0] = s_resultBuf[0];
        s_resultChannel[0] = 0; // sentinel — "nothing to hand off", see wifi_scan.h
        s_resultCount = 1;
    }
}

static void wifiScanWorkerTask(void *param) {
    (void)param;

    int n = WIFI_SCAN_FAILED;
    if (WiFi.mode(WIFI_STA)) {
        WiFi.disconnect();
        delay(100);
        n = WiFi.scanNetworks(true /* async */);
    }

    // Poll for scan completion while staying responsive to an early STOP.
    for (;;) {
        if (n != WIFI_SCAN_RUNNING) {
            if (n >= 0) {
                formatResults(n);
            } else {
                snprintf(s_resultBuf[0], RESULT_LINE_LEN, "Scan failed; BACK to retry");
                s_resultPtrs[0] = s_resultBuf[0];
                s_resultChannel[0] = 0;
                s_resultCount = 1;
            }
            RenderMsg msg;
            msg.kind = RENDER_MSG_SCAN_COMPLETE;
            messagingPostTelemetry(msg);
            break;
        }

        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            goto cleanup;
        }
        n = WiFi.scanComplete();
    }

    // Results are up; just wait here until told to stop.
    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, portMAX_DELAY) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }
    }
cleanup:
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void wifiScanStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[WIFI_SCAN] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_resultCount = 0;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(wifiScanWorkerTask, "wifi_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void wifiScanStop() {
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

const char *const *wifiScanGetResultLines() {
    return s_resultPtrs;
}

int wifiScanGetResultCount() {
    return s_resultCount;
}

const uint8_t *wifiScanGetResultBSSID(int index) {
    return s_resultBssid[index];
}

const char *wifiScanGetResultSSID(int index) {
    return s_resultSsid[index];
}

uint8_t wifiScanGetResultChannel(int index) {
    return s_resultChannel[index];
}
