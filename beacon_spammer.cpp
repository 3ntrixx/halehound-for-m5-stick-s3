// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Beacon Spammer — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "beacon_spammer.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define BEACON_STATUS_INTERVAL_MS 250

// SSID list — identical to wifi_attacks.cpp's BeaconSpammer::ssidList.
static const char *const BEACON_SSID_LIST[] = {
    "404_SSID_Not_Found", "Free_WiFi_Promise", "PrettyFlyForAWiFi", "Wi-Fight_The_Power",
    "Tell_My_WiFi_LoveHer", "Wu-Tang_LAN", "LAN_of_the_Free", "No_More_Data",
    "Panic!_At_the_WiFi", "HideYoKidsHideYoWiFi", "Definitely_Not_A_Spy", "Click_and_Die",
    "DropItLikeItsHotspot", "Loading...", "I_AM_Watching_You", "Why_Tho?",
    "Get_Your_Own_WiFi", "NSA_Surveillance_Van", "WiFi_Fairy", "Undercover_Potato",
    "TheLANBeforeTime", "ItHurtsWhen_IP", "IPFreely", "NoInternetHere",
    "LookMaNoCables", "Router?IHardlyKnewHer", "ShutUpAndConnect", "Mom_UseThisOne",
    "Not_for_You", "OopsAllSSID", "ItsOver9000", "Bob's_Wifi_Burgers",
    "Overclocked_Toaster", "Pikachu_Used_WiFi", "Cheese_Bandit", "Quantum_Tunnel",
    "Meme_LANd", "HaleHound_Was_Here", "Fenrir_Network", "DRAUGR_AP",
};
static const int BEACON_SSID_COUNT = sizeof(BEACON_SSID_LIST) / sizeof(BEACON_SSID_LIST[0]);

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

static TaskHandle_t s_txTaskHandle = NULL;
static volatile bool s_txRunning = false;
static volatile bool s_txDone = false;
static volatile int s_channel = 1;
static volatile bool s_nukeMode = false;
static volatile uint32_t s_txCount = 0;

// Beacon frame template — identical to wifi_attacks.cpp's packet[]/nukePacket[]
// (byte-for-byte the same initial contents; Normal and Nuke mode never run
// concurrently in this single-task design, so one shared buffer is enough —
// a documented simplification of the original's two-buffer, two-task split).
static uint8_t s_packet[128] = {
    0x80, 0x00, 0x00, 0x00,                         // Frame control + duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination (broadcast)
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // Source (random per packet)
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // BSSID (random per packet)
    0xc0, 0x6c,                                     // Sequence/fragment
    0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00, 0x00, 0x00, // Timestamp
    0x64, 0x00,                                     // Beacon interval
    0x01, 0x04,                                     // Capability info
    0x00, 0x06, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, // SSID element (overwritten per packet)
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, // Supported rates
    0x03, 0x01, 0x04                                // DS Parameter set (channel)
};

// Raw ESP-IDF APSTA + hidden AP — same proven init/teardown shape as
// deauther.cpp's initAttackMode()/exitAttackMode(), including the explicit
// stop+deinit reset before re-init and reliance on deauther.cpp's global
// ieee80211_raw_frame_sanity_check() override (both needed for
// esp_wifi_80211_tx(WIFI_IF_AP, ...) to actually transmit on this framework
// version). Not ported: the original's esp_wifi_set_promiscuous(true) call —
// deauther.cpp already established that's unnecessary for TX-only use.
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

// Core 0 tight-loop TX task — ports both bsTxTask() (Normal) and
// nukeTxTask() (Nuke) into one task, switching on s_nukeMode each iteration
// (matches the original's own volatile-shared-state design: Core 1 writes
// bsChannel on touch, Core 0 reads it every iteration with no queue).
//
// Efficiency changes vs. the original (which this port copied verbatim):
//  - esp_wifi_set_channel() is only called when the channel actually
//    changes. The original retuned on every single packet, and a retune
//    drains the raw-TX queue — most of the loop was spent off-channel.
//    Nuke mode still hops per packet (that's its point).
//  - esp_wifi_80211_tx()'s return value is checked. On a full queue the
//    task yields one tick and retries, instead of the original's blind 3x
//    duplicate send that mostly just filled the shallow queue.
//  - Random SA/BSSID bytes are forced to valid unicast, locally-administered
//    addresses (clear the multicast bit) — scanners ignore frames whose
//    BSSID has the multicast bit set, which made ~half of all frames
//    invisible on the original.
static void beaconTxTask(void *param) {
    (void)param;

    static const char charPool[] = "1234567890qwertyuiopasdfghjklzxcvbnmQWERTYUIOPASDFGHJKLZXCVBNM_";
    static const int charPoolLen = sizeof(charPool) - 1;

    int yieldCounter = 0;
    uint8_t appliedChannel = 0; // forces the first set_channel
    uint16_t seq = 0;

    while (s_txRunning) {
        uint8_t channel;
        int packetSize;

        if (s_nukeMode) {
            // Random channel every packet — matches nukeTxTask()'s chaos hop.
            channel = random(1, 13);

            for (int i = 10; i <= 21; i++) {
                s_packet[i] = random(256);
            }
            s_packet[10] = (s_packet[10] & 0xFE) | 0x02; // SA: unicast + locally administered
            s_packet[16] = (s_packet[16] & 0xFE) | 0x02; // BSSID: same

            for (int i = 38; i <= 43; i++) {
                s_packet[i] = charPool[random(charPoolLen)];
            }
            s_packet[37] = 6; // fixed SSID length
            s_packet[56] = channel;

            packetSize = 57;
        } else {
            channel = (uint8_t)s_channel;

            for (int i = 10; i <= 21; i++) {
                s_packet[i] = random(256);
            }
            s_packet[10] = (s_packet[10] & 0xFE) | 0x02; // SA: unicast + locally administered
            s_packet[16] = (s_packet[16] & 0xFE) | 0x02; // BSSID: same

            const char *ssid = BEACON_SSID_LIST[random(BEACON_SSID_COUNT)];
            int ssidLength = strlen(ssid);
            if (ssidLength > 32) ssidLength = 32;

            s_packet[37] = ssidLength;
            for (int i = 0; i < ssidLength; i++) {
                s_packet[38 + i] = ssid[i];
            }

            int ratesOffset = 38 + ssidLength;
            s_packet[ratesOffset]     = 0x01;
            s_packet[ratesOffset + 1] = 0x08;
            s_packet[ratesOffset + 2] = 0x82;
            s_packet[ratesOffset + 3] = 0x84;
            s_packet[ratesOffset + 4] = 0x8b;
            s_packet[ratesOffset + 5] = 0x96;
            s_packet[ratesOffset + 6] = 0x24;
            s_packet[ratesOffset + 7] = 0x30;
            s_packet[ratesOffset + 8] = 0x48;
            s_packet[ratesOffset + 9] = 0x6c;

            int dsOffset = ratesOffset + 10;
            s_packet[dsOffset]     = 0x03;
            s_packet[dsOffset + 1] = 0x01;
            s_packet[dsOffset + 2] = channel;

            packetSize = dsOffset + 3;
        }

        // Increment the 12-bit sequence number per frame, like a real AP.
        s_packet[22] = (seq & 0x0F) << 4;
        s_packet[23] = (seq >> 4) & 0xFF;
        seq = (seq + 1) & 0x0FFF;

        if (channel != appliedChannel) {
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
            appliedChannel = channel;
        }

        // One unique frame per iteration; retry only on a real driver
        // failure (full TX queue), yielding so the queue can drain.
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, s_packet, packetSize, false);
        if (err == ESP_OK) {
            s_txCount++;
        } else {
            vTaskDelay(1);
        }

        // Feed watchdog — yield every 50 iterations.
        yieldCounter++;
        if (yieldCounter >= 50) {
            yieldCounter = 0;
            vTaskDelay(1);
        }
    }

    s_txDone = true;
    vTaskDelete(NULL);
}

// Outer command/status task — starts the TX task, polls s_cmdQueue for
// STOP, posts a status line periodically. Same Core0-TX/outer-status split
// already used by deauther.cpp's attackWorkerTask().
static void beaconSpammerWorkerTask(void *param) {
    (void)param;

    s_txCount = 0;
    s_nukeMode = false;
    s_channel = 1;

    initAttackMode();

    s_txDone = false;
    s_txRunning = true;
    xTaskCreatePinnedToCore(beaconTxTask, "beacon_tx", 8192, NULL, 1, &s_txTaskHandle, 0);

    uint32_t lastStatus = 0;

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }

        uint32_t now = millis();
        if (now - lastStatus >= BEACON_STATUS_INTERVAL_MS) {
            lastStatus = now;
            RenderMsg msg;
            msg.kind = RENDER_MSG_STATUS_TEXT;
            if (s_nukeMode) {
                snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "NUKE TX:%lu", (unsigned long)s_txCount);
            } else {
                snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "Spam CH%d TX:%lu", s_channel, (unsigned long)s_txCount);
            }
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

void beaconSpammerStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[BEACON_SPAMMER] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(beaconSpammerWorkerTask, "beacon_spam", 4096, NULL, 2, &s_workerHandle, 0);
}

void beaconSpammerStop() {
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

void beaconSpammerAdjustChannel(int delta) {
    if (s_nukeMode) return; // matches the original: CH-/CH+ only ever touched bsChannel, which nukeTxTask never reads.

    int ch = s_channel;
    if (delta > 0) {
        ch = (ch >= 14) ? 1 : ch + 1;
    } else if (delta < 0) {
        ch = (ch <= 1) ? 14 : ch - 1;
    }
    s_channel = ch;
}

void beaconSpammerToggleNuke() {
    s_nukeMode = !s_nukeMode;
}
