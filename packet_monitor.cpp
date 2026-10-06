// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Packet Monitor — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 4)
//
// Promiscuous-mode init/cleanup sequence (retry loop, WIFI_STORAGE_RAM,
// mode NULL then start) mirrors the pattern already proven reliable in
// wifi_attacks.cpp's wifiPromiscuousInit()/wifiCleanup() — reimplemented
// small and self-contained here rather than pulling in that whole
// (CYD-touch-UI-coupled) 7790-line file for two functions.
// ═══════════════════════════════════════════════════════════════════════════

#include "packet_monitor.h"
#include <WiFi.h>
#include <arduinoFFT.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;
static volatile uint32_t s_packetCount = 0;
static volatile uint32_t s_deauthCount = 0;
static volatile uint32_t s_framePacketCount = 0;
static int s_requestedChannel = 1;

// Same transform and sampling cadence as the original CYD PacketMonitor.
#define PM_FFT_SAMPLES 256
static const double PM_SAMPLING_HZ = 5000.0;
static double s_vReal[PM_FFT_SAMPLES];
static double s_vImag[PM_FFT_SAMPLES];
static ArduinoFFT<double> s_fft(s_vReal, s_vImag, PM_FFT_SAMPLES, PM_SAMPLING_HZ);
static double s_attenuation = 10.0;

static void IRAM_ATTR promiscRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Deauth (0xC0) / disassoc (0xA0) frame detection — matches
    // wifi_attacks.cpp's PacketMonitor namespace exactly (same two
    // frame-control subtype bytes checked on a management frame).
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (type == WIFI_PKT_MGMT && (pkt->payload[0] == 0xA0 || pkt->payload[0] == 0xC0)) {
        __atomic_fetch_add(&s_deauthCount, 1, __ATOMIC_RELAXED);
    }
    if (type == WIFI_PKT_MISC || pkt->rx_ctrl.sig_len > 2324) return;
    __atomic_fetch_add(&s_packetCount, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_framePacketCount, 1, __ATOMIC_RELAXED);
}

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
        Serial.println("[PKT_MON] promiscuous init FAILED after 3 attempts");
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

static void packetMonitorWorkerTask(void *param) {
    (void)param;

    if (!promiscuousInit()) {
        RenderMsg msg = {};
        msg.kind = RENDER_MSG_STATUS_TEXT;
        strlcpy(msg.payload.text, "WiFi monitor init failed", sizeof(msg.payload.text));
        messagingPostTelemetry(msg);
        radioRelease(RADIO_WIFI);
        xSemaphoreGive(s_doneSem);
        vTaskDelete(NULL);
        return;
    }

    int channel = 1;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_packetCount = 0;
    s_deauthCount = 0;
    s_framePacketCount = 0;
    s_attenuation = 10.0;
    const uint32_t samplePeriodUs = (uint32_t)(1000000.0 / PM_SAMPLING_HZ);

    for (;;) {
        Cmd cmd;
        while (xQueueReceive(s_cmdQueue, &cmd, 0) == pdTRUE) {
            if (cmd.type == CMD_STOP) goto cleanup;
            if (cmd.type == CMD_SET_CHANNEL) {
                channel = cmd.intArg;
                if (channel < 1) channel = 13;
                if (channel > 13) channel = 1;
                esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            }
        }

        uint32_t nextSample = micros();
        for (int i = 0; i < PM_FFT_SAMPLES; i++) {
            s_vReal[i] = __atomic_load_n(&s_framePacketCount, __ATOMIC_RELAXED) * 300.0;
            s_vImag[i] = 1.0;
            nextSample += samplePeriodUs;
            while ((int32_t)(micros() - nextSample) < 0) { }
        }

        uint32_t framePackets = __atomic_exchange_n(&s_framePacketCount, 0, __ATOMIC_RELAXED);
        double mean = 0;
        for (int i = 0; i < PM_FFT_SAMPLES; i++) mean += s_vReal[i];
        mean /= PM_FFT_SAMPLES;
        for (int i = 0; i < PM_FFT_SAMPLES; i++) s_vReal[i] -= mean;

        s_fft.windowing(FFTWindow::Hamming, FFTDirection::Forward);
        s_fft.compute(FFTDirection::Forward);
        s_fft.complexToMagnitude();

        RenderMsg msg = {};
        msg.kind = RENDER_MSG_WIFI_MONITOR;
        msg.payload.wifiMonitor.totalPackets = __atomic_load_n(&s_packetCount, __ATOMIC_RELAXED);
        msg.payload.wifiMonitor.deauthPackets = __atomic_load_n(&s_deauthCount, __ATOMIC_RELAXED);
        msg.payload.wifiMonitor.packetsThisFrame = (framePackets > 65535) ? 65535 : (uint16_t)framePackets;
        msg.payload.wifiMonitor.channel = channel;

        double maxMagnitude = 0;
        for (int j = 0; j < WIFI_MONITOR_BINS; j++) {
            int fftIndex = (j * (PM_FFT_SAMPLES / 2)) / WIFI_MONITOR_BINS;
            double magnitude = s_vReal[fftIndex];
            if (magnitude > maxMagnitude) maxMagnitude = magnitude;
            int level = (int)(magnitude / s_attenuation);
            msg.payload.wifiMonitor.spectrum[j] = (uint8_t)constrain(level, 0, 127);
        }
        double neededAttenuation = maxMagnitude / 127.0;
        if (neededAttenuation > s_attenuation) s_attenuation = neededAttenuation;
        messagingPostTelemetry(msg);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

cleanup:
    promiscuousCleanup();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void packetMonitorAdjustChannel(int delta) {
    if (!s_cmdQueue || delta == 0) return;
    s_requestedChannel += (delta > 0) ? 1 : -1;
    if (s_requestedChannel < 1) s_requestedChannel = 13;
    if (s_requestedChannel > 13) s_requestedChannel = 1;
    Cmd cmd = {};
    cmd.type = CMD_SET_CHANNEL;
    cmd.intArg = s_requestedChannel;
    xQueueSend(s_cmdQueue, &cmd, portMAX_DELAY);
}

void packetMonitorStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[PKT_MON] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_requestedChannel = 1;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(packetMonitorWorkerTask, "pkt_monitor", 6144, NULL, 2, &s_workerHandle, 0);
}

void packetMonitorStop() {
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
