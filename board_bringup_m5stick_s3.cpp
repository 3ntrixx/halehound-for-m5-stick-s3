// ═══════════════════════════════════════════════════════════════════════════
// HaleHound M5Stack StickS3 — Orchestrator
// Created: 2026-10-04 (StickS3 port, Stages 1-4 — board bring-up +
// on-chip WiFi/BLE features; see PORT_M5StickS3_WiFi-BLE_Outline.md)
//
// Modeled directly on board_bringup_cardputer.cpp — same NavState shape,
// same messaging.h/ui_render.h/menu_data.h seams, same WiFi/BLE-only
// feature boundary (identical 240x135 panel too), with button_input.*
// standing in for keyboard_input.* as the InputEvent producer. Everything
// downstream of messaging.h's five-event model is unchanged.
//
// Only compiled for [env:m5stick-s3] (see platformio.ini build_src_filter)
// — this file provides its own setup()/loop(), like the other two board
// bring-up files do for their own envs.
//
// NOT hardware-tested — see the outline's lifecycle/regression milestone
// (§7) before treating any of this as verified on real StickS3 hardware.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include "cyd_config.h"
#include "board_power_m5stick_s3.h"
#include "button_input.h"
#include "input_task.h"
#include "ui_render.h"
#include "menu_data.h"
#include "spi_manager.h"
#include "radio_manager.h"
#include "i2c_manager.h"
#include "messaging.h"
#include "psram_pool.h"
#include "wifi_scan.h"
#include "packet_monitor.h"
#include "ble_scan.h"
#include "ble_skeletonkey.h"
#include "ble_spoofer.h"
#include "ble_beacon.h"
#include "lunatic_fringe.h"
#include "airtag_tracker.h"
#include "storage_manager.h"
#include "eapol_sniff.h"
#include "wardrive_lite.h"
#include "karma_attack_tembed.h"
#include "saved_captures_tembed.h"
#include "iot_recon_tembed.h"
#include "loot_viewer_tembed.h"
#include "flock_you_tembed.h"
#include "deauther.h"
#include "wifi_guardian.h"
#include "probe_sniffer.h"
#include "beacon_spammer.h"
#include "auth_flood.h"
#include "station_scan.h"
#include "captive_portal.h"
#include "settings_manager.h"
#include "battery_gauge.h"

enum NavState {
    NAV_MAIN,
    NAV_SUBMENU,
    NAV_PLACEHOLDER,
    NAV_WIFI_SCAN_RUNNING,
    NAV_WIFI_SCAN_RESULTS,
    NAV_PACKET_MONITOR_RUNNING,
    NAV_BLE_SCAN_RUNNING,
    NAV_BLE_SCAN_RESULTS,
    NAV_BLE_SKELETONKEY_CONNECTING,
    NAV_BLE_SKELETONKEY_RESULTS,
    NAV_BLE_SKELETONKEY_CONTROL,
    NAV_BLE_SPOOFER_MENU,
    NAV_BLE_SPOOFER_RUNNING,
    NAV_BLE_BEACON_RUNNING,
    NAV_LUNATIC_RUNNING,
    NAV_LUNATIC_RESULTS,
    NAV_AIRTAG_RUNNING,
    NAV_AIRTAG_RESULTS,
    NAV_EAPOL_SCAN_RUNNING,
    NAV_EAPOL_AP_LIST,
    NAV_EAPOL_CAPTURE_RUNNING,
    NAV_WARDRIVE_LITE_RUNNING,
    NAV_KARMA_RUNNING,
    NAV_SAVED_CAPTURES_BUSY,
    NAV_SAVED_CAPTURES_LIST,
    NAV_SAVED_CAPTURES_VIEW,
    NAV_IOT_RECON_RUNNING,
    NAV_LOOT_BUSY,
    NAV_LOOT_HUB,
    NAV_LOOT_FILE_LIST,
    NAV_LOOT_FILE_VIEW,
    NAV_FLOCK_YOU_RUNNING,
    NAV_DEAUTHER_SCAN_RUNNING,
    NAV_DEAUTHER_AP_LIST,
    NAV_DEAUTHER_ATTACK_RUNNING,
    NAV_WIFI_GUARDIAN_RUNNING,
    NAV_PROBE_SNIFFER_RUNNING,
    NAV_PROBE_SNIFFER_RESULTS,
    NAV_BEACON_SPAMMER_RUNNING,
    NAV_AUTH_FLOOD_SCAN_RUNNING,
    NAV_AUTH_FLOOD_AP_LIST,
    NAV_AUTH_FLOOD_ATTACK_RUNNING,
    NAV_STATION_SCAN_RUNNING,
    NAV_STATION_SCAN_RESULTS,
    NAV_CAPTIVE_PORTAL_READY,
    NAV_CAPTIVE_PORTAL_EDIT_SSID,
    NAV_CAPTIVE_PORTAL_STARTING,
    NAV_CAPTIVE_PORTAL_ACTIVE,
    NAV_SETTINGS_BRIGHTNESS,
    NAV_SETTINGS_TIMEOUT,
    NAV_SETTINGS_SWAP,
    NAV_SETTINGS_INVERT,
    NAV_SETTINGS_COLOR_MODE,
    NAV_SETTINGS_ROTATION,
    NAV_SETTINGS_DEVICE_INFO,
    NAV_SETTINGS_PIN,
    NAV_ABOUT,
    NAV_LOCKED,
    NAV_PIN_ENTRY,
};

static NavState s_navState = NAV_MAIN;
static int s_mainIndex = 0;
static int s_subIndex = 0;
static int s_resultIndex = 0;
static bool s_portalFromKarma = false;

// ── SkeletonKey light-control scratch state ─────────────────────────────
// Brightness is a stateless percentage the protocol accepts per-command
// (no readback exists for most of these controllers), so the UI has to
// remember the last value it sent and step it; reset to 100 on every fresh
// connect/rescan so a new device doesn't inherit a stale level.
static int s_skBrightness = 100;
// "Color Mix" (the reference SkeletonKey app's 7th command, between WHITE
// and View raw GATT) sends Action::Color with an arbitrary RGB triple —
// the one Action the 1-6 single-channel rows above can't reach. There's no
// color wheel on this board's two buttons, so each tap cycles to the next
// preset and fires it immediately, same "tap to fire" shape as every other
// row here.
struct SkColorPreset { const char *name; uint8_t r, g, b; };
static const SkColorPreset SK_COLOR_MIX[] = {
    {"Orange", 255, 80, 0}, {"Purple", 160, 0, 255}, {"Cyan", 0, 255, 255},
    {"Yellow", 255, 220, 0}, {"Pink", 255, 0, 120}, {"Teal", 0, 200, 160},
};
static constexpr int SK_COLOR_MIX_COUNT = sizeof(SK_COLOR_MIX) / sizeof(SK_COLOR_MIX[0]);
static int s_skColorIndex = 0;
// Number of selectable rows on the SkeletonKey light-control screen
// (showCurrentScreen's NAV_BLE_SKELETONKEY_CONTROL case); kept beside
// moveSelection's wrap-around math so both stay in sync.
static constexpr int SK_CONTROL_ITEMS = 12;
// SkeletonKeySnapshot is ~8KB (SK_MAX_ROWS row/uuid/value tables) — one
// shared static instance, not a local in every call site, since everything
// touching it runs sequentially on this one task (board_bringup has no
// separate render task; see showCurrentScreen()/loop()).
static SkeletonKeySnapshot s_skSnapshot;

// Finds the next profile index (wrapping) whose GATT endpoint matched this
// device, starting after `current`. Returns -1 if the mask is empty.
static int skNextCompatibleProfile(uint32_t mask, int current) {
    if (!mask) return -1;
    for (int step = 1; step <= SkeletonKeyProtocol::ProfileCount; ++step) {
        int idx = (current + step) % SkeletonKeyProtocol::ProfileCount;
        if (mask & (uint32_t(1) << idx)) return idx;
    }
    return -1;
}

// ── Settings screen scratch state ───────────────────────────────────────
static int s_timeoutIndex = 0;
static int s_colorModeIndex = 0;
static int s_rotationIndex = 0;

// ── PIN lock / PIN setup digit wheel ────────────────────────────────────
static uint8_t s_pinDigits[4] = {0, 0, 0, 0};
static int s_pinPos = 0;
static char s_pinMask[6] = "____";

static void updatePinMask() {
    for (int i = 0; i < 4; i++) {
        s_pinMask[i] = (i < s_pinPos) ? (char)('0' + s_pinDigits[i]) : '_';
    }
    s_pinMask[4] = '\0';
}

static uint16_t pinFromDigits() {
    return (uint16_t)(s_pinDigits[0] * 1000 + s_pinDigits[1] * 100 +
                      s_pinDigits[2] * 10 + s_pinDigits[3]);
}

// ── Screen timeout idle blank ───────────────────────────────────────────
static uint32_t s_lastInputMs = 0;
static bool s_screenBlanked = false;

static bool isFeatureRunning() {
    // NAV_BLE_SCAN_RESULTS/NAV_LUNATIC_RESULTS/NAV_PROBE_SNIFFER_RESULTS/
    // NAV_STATION_SCAN_RESULTS are included here (unlike WIFI/EAPOL's
    // *_RESULTS states) because those workers never park after posting
    // results — they keep passively listening and live-updating the table
    // the whole time the list is on screen (see ble_scan.cpp et al).
    return s_navState == NAV_WIFI_SCAN_RUNNING || s_navState == NAV_PACKET_MONITOR_RUNNING ||
           s_navState == NAV_BLE_SCAN_RUNNING || s_navState == NAV_BLE_SCAN_RESULTS ||
           s_navState == NAV_BLE_SKELETONKEY_CONNECTING ||
           // RESULTS/CONTROL are included (unlike WIFI/EAPOL's *_RESULTS
           // states) so a Read or a light command's async ACK/error status
           // reaches the screen within one telemetry poll instead of only
           // on the user's next button press.
           s_navState == NAV_BLE_SKELETONKEY_RESULTS || s_navState == NAV_BLE_SKELETONKEY_CONTROL ||
           s_navState == NAV_BLE_SPOOFER_RUNNING || s_navState == NAV_BLE_BEACON_RUNNING ||
           s_navState == NAV_LUNATIC_RUNNING || s_navState == NAV_LUNATIC_RESULTS ||
           s_navState == NAV_AIRTAG_RUNNING || s_navState == NAV_AIRTAG_RESULTS ||
           s_navState == NAV_EAPOL_SCAN_RUNNING || s_navState == NAV_EAPOL_CAPTURE_RUNNING ||
           s_navState == NAV_WARDRIVE_LITE_RUNNING || s_navState == NAV_KARMA_RUNNING ||
           s_navState == NAV_SAVED_CAPTURES_BUSY || s_navState == NAV_SAVED_CAPTURES_LIST ||
           s_navState == NAV_SAVED_CAPTURES_VIEW || s_navState == NAV_IOT_RECON_RUNNING ||
           s_navState == NAV_LOOT_BUSY || s_navState == NAV_LOOT_HUB ||
           s_navState == NAV_LOOT_FILE_LIST || s_navState == NAV_LOOT_FILE_VIEW ||
           s_navState == NAV_FLOCK_YOU_RUNNING ||
           s_navState == NAV_DEAUTHER_SCAN_RUNNING || s_navState == NAV_DEAUTHER_ATTACK_RUNNING ||
           s_navState == NAV_WIFI_GUARDIAN_RUNNING ||
           s_navState == NAV_PROBE_SNIFFER_RUNNING || s_navState == NAV_PROBE_SNIFFER_RESULTS ||
           s_navState == NAV_BEACON_SPAMMER_RUNNING ||
           s_navState == NAV_AUTH_FLOOD_SCAN_RUNNING || s_navState == NAV_AUTH_FLOOD_ATTACK_RUNNING ||
           s_navState == NAV_STATION_SCAN_RUNNING || s_navState == NAV_STATION_SCAN_RESULTS ||
           // NAV_CAPTIVE_PORTAL_READY/_EDIT_SSID deliberately excluded —
           // pure state, no radio touched yet.
           s_navState == NAV_CAPTIVE_PORTAL_STARTING || s_navState == NAV_CAPTIVE_PORTAL_ACTIVE;
}

// Surface the queue overrun counters in every running feature's status
// line, centralized so no module has to know about them.
static void showFeatureStatus(const char *title, const char *text) {
    uint32_t drops = droppedInputEvents + droppedCaptureMsgs + droppedLogMsgs;
    if (drops == 0) {
        uiRenderShowStatus(title, text);
        return;
    }
    char buf[RENDER_MSG_TEXT_MAX];
    snprintf(buf, sizeof(buf), "%s [D:%lu]", text, (unsigned long)drops);
    uiRenderShowStatus(title, buf);
}

static void showWifiFeatureStatus(const char *title, const char *text, const char *hint) {
    uint32_t drops = droppedInputEvents + droppedCaptureMsgs + droppedLogMsgs;
    if (drops == 0) {
        uiRenderShowWifiStatus(title, text, hint);
        return;
    }
    char buf[RENDER_MSG_TEXT_MAX];
    snprintf(buf, sizeof(buf), "%s [D:%lu]", text, (unsigned long)drops);
    uiRenderShowWifiStatus(title, buf, hint);
}

// CYD-parity per-submenu icon sets (menu_data.h's CARDPUTER_* tables — this
// board reuses them verbatim, see menu_data.h's BOARD_M5STICK_S3 note).
static const unsigned char *const *submenuIconsFor(int mainIndex) {
    switch (mainIndex) {
        case CARDPUTER_WIFI_MENU_INDEX:      return WIFI_SUBMENU_ICONS;
        case CARDPUTER_BT_MENU_INDEX:        return CARDPUTER_BT_SUBMENU_ICONS;
        case CARDPUTER_JAMDETECT_MENU_INDEX: return CARDPUTER_JAMDETECT_SUBMENU_ICONS;
        case CARDPUTER_SIGINT_MENU_INDEX:    return CARDPUTER_SIGINT_SUBMENU_ICONS;
        case CARDPUTER_SETTINGS_MENU_INDEX:  return CARDPUTER_SETTINGS_SUBMENU_ICONS;
        case CARDPUTER_ABOUT_MENU_INDEX:     return ABOUT_SUBMENU_ICONS;
        default:                             return NULL;
    }
}

static void showCurrentScreen() {
    switch (s_navState) {
        case NAV_MAIN:
            uiRenderShowMainMenu(s_mainIndex);
            break;
        case NAV_SUBMENU:
            uiRenderShowSubmenuIcons(CARDPUTER_MAIN_MENU[s_mainIndex].title,
                                     CARDPUTER_MAIN_MENU[s_mainIndex].items,
                                     submenuIconsFor(s_mainIndex),
                                     CARDPUTER_MAIN_MENU[s_mainIndex].count,
                                     s_subIndex);
            break;
        case NAV_PLACEHOLDER:
            uiRenderShowPlaceholder(CARDPUTER_MAIN_MENU[s_mainIndex].items[s_subIndex]);
            break;
        case NAV_LUNATIC_RUNNING:
            showFeatureStatus("Tracker Scan", "Scanning...");
            break;
        case NAV_LUNATIC_RESULTS:
            uiRenderShowSubmenu("Trackers", lunaticFringeGetResultLines(),
                                lunaticFringeGetResultCount(), s_resultIndex);
            break;
        case NAV_AIRTAG_RUNNING:
            showFeatureStatus("AirTag Tracker", "Scanning...");
            break;
        case NAV_AIRTAG_RESULTS:
            uiRenderShowSubmenu("AirTags", airtagTrackerGetResultLines(),
                                airtagTrackerGetResultCount(), s_resultIndex);
            break;
        case NAV_WIFI_SCAN_RUNNING:
            showWifiFeatureStatus("WIFI SCAN", "Scanning networks...", "Hold B Cancel");
            break;
        case NAV_WIFI_SCAN_RESULTS:
            uiRenderShowWifiList("WIFI SCAN", wifiScanGetResultLines(),
                                 wifiScanGetResultCount(), s_resultIndex,
                                 "Hold A Select | Hold B Back");
            break;
        case NAV_PACKET_MONITOR_RUNNING:
            showWifiFeatureStatus("PKT MONITOR", "Starting monitor...", "A/B Channel | B-hold Back");
            break;
        case NAV_BLE_SCAN_RUNNING:
            showFeatureStatus("BLE Predator", "Scanning...");
            break;
        case NAV_BLE_SCAN_RESULTS: {
            static BleDisplayRow bleRows[UI_BLE_MAX_ROWS];
            static BleDisplayStats bleStats;
            bleScanGetDisplayRows(bleRows, UI_BLE_MAX_ROWS, &bleStats);
            uiRenderShowBleDevices(bleRows, &bleStats, s_resultIndex);
            break;
        }
        case NAV_BLE_SKELETONKEY_CONNECTING: {
            skeletonKeyGetSnapshot(&s_skSnapshot);
            showFeatureStatus("SkeletonKey", s_skSnapshot.status[0] ? s_skSnapshot.status
                               : (skeletonKeyGetState() == SK_STATE_FAILED
                                      ? "Connect failed [hold B]" : "Connecting..."));
            break;
        }
        case NAV_BLE_SKELETONKEY_RESULTS: {
            skeletonKeyGetSnapshot(&s_skSnapshot);
            static char skTitle[40];
            snprintf(skTitle, sizeof(skTitle), "SK: %.33s", s_skSnapshot.device);
            uiRenderShowSubmenu(skTitle, skeletonKeyGetRowLines(),
                                skeletonKeyGetRowCount(), s_resultIndex,
                                s_skSnapshot.status[0] ? s_skSnapshot.status : "Hold B Rescan");
            break;
        }
        case NAV_BLE_SKELETONKEY_CONTROL: {
            skeletonKeyGetSnapshot(&s_skSnapshot);
            static char skTitle[40];
            static char skLines[SK_CONTROL_ITEMS][40];
            static const char *skPtrs[SK_CONTROL_ITEMS];
            const SkeletonKeyProtocol::Profile *prof =
                s_skSnapshot.profileIndex >= 0 ? SkeletonKeyProtocol::profile(s_skSnapshot.profileIndex) : nullptr;
            if (prof) snprintf(skTitle, sizeof(skTitle), "%.20s: %.18s", prof->name, s_skSnapshot.device);
            else snprintf(skTitle, sizeof(skTitle), "SK: %.33s", s_skSnapshot.device);
            snprintf(skLines[0], 40, "Power ON");
            snprintf(skLines[1], 40, "Power OFF");
            snprintf(skLines[2], 40, "Red");
            snprintf(skLines[3], 40, "Green");
            snprintf(skLines[4], 40, "Blue");
            snprintf(skLines[5], 40, "White");
            snprintf(skLines[6], 40, "Color Mix (%s)", SK_COLOR_MIX[s_skColorIndex].name);
            snprintf(skLines[7], 40, "Brightness - (%d%%)", s_skBrightness);
            snprintf(skLines[8], 40, "Brightness + (%d%%)", s_skBrightness);
            snprintf(skLines[9], 40, "Profile: %.20s", prof ? prof->name : "(none selected)");
            snprintf(skLines[10], 40, "View raw GATT (%d rows)", s_skSnapshot.rowCount);
            snprintf(skLines[11], 40, "Rescan");
            for (int i = 0; i < SK_CONTROL_ITEMS; i++) skPtrs[i] = skLines[i];
            uiRenderShowSubmenu(skTitle, skPtrs, SK_CONTROL_ITEMS, s_resultIndex,
                                s_skSnapshot.status[0] ? s_skSnapshot.status : "A/B Browse | Hold A Send");
            break;
        }
        case NAV_BLE_SPOOFER_MENU: {
            // Payload picker — a selectable list of every spam mode. Selecting
            // a row launches that mode (see handleSelect). Built from the BLE
            // spoofer's own mode table so the two can't drift out of sync.
            int count = bleSpooferModeCount();
            static const char *spoofPtrs[16];
            if (count > 16) count = 16;
            for (int i = 0; i < count; i++) spoofPtrs[i] = bleSpooferModeName(i);
            uiRenderShowSubmenu("BLE Spoofer", spoofPtrs, count, s_resultIndex,
                                "Select a payload to launch");
            break;
        }
        case NAV_BLE_SPOOFER_RUNNING:
            showFeatureStatus("BLE Spoofer", "starting...");
            break;
        case NAV_BLE_BEACON_RUNNING:
            showFeatureStatus("BLE Beacon", "starting...");
            break;
        case NAV_EAPOL_SCAN_RUNNING:
            showFeatureStatus("EAPOL Capture", "Scanning...");
            break;
        case NAV_EAPOL_AP_LIST:
            uiRenderShowSubmenu("EAPOL Targets", eapolSniffGetApResultLines(),
                                eapolSniffGetApResultCount(), s_resultIndex);
            break;
        case NAV_EAPOL_CAPTURE_RUNNING:
            showFeatureStatus("EAPOL Capture", "starting...");
            break;
        case NAV_WARDRIVE_LITE_RUNNING:
            showFeatureStatus("Wardriving", "Scanning...");
            break;
        case NAV_KARMA_RUNNING:
            uiRenderShowSubmenu("Karma", karmaGetResultLines(),
                                karmaGetResultCount(), s_resultIndex,
                                "A/B Browse | Hold A Twin | Hold B Back");
            break;
        case NAV_SAVED_CAPTURES_BUSY:
            showFeatureStatus("Captures", "working...");
            break;
        case NAV_SAVED_CAPTURES_LIST:
            uiRenderShowSubmenu(savedCapturesGetTitle(), savedCapturesGetFileListLines(),
                                savedCapturesGetFileListCount(), s_resultIndex,
                                "A/B Browse | Hold A View | Hold B Back");
            break;
        case NAV_SAVED_CAPTURES_VIEW:
            uiRenderShowSubmenu(savedCapturesGetTitle(), savedCapturesGetFileViewLines(),
                                savedCapturesGetFileViewCount(), s_resultIndex,
                                "A/B Scroll | Hold B Back");
            break;
        case NAV_IOT_RECON_RUNNING: {
            char hdr[32];
            snprintf(hdr, sizeof(hdr), "IoT C:%d H:%d",
                     iotReconGetCamCount(), iotReconGetSmartHubCount());
            uiRenderShowSubmenu(hdr, iotReconGetResultLines(),
                                iotReconGetResultCount(), s_resultIndex,
                                "A/B Browse | LIVE | Hold B Back");
            break;
        }
        case NAV_LOOT_BUSY:
            showFeatureStatus("Loot", "working...");
            break;
        case NAV_LOOT_HUB:
            uiRenderShowSubmenu(lootViewerGetTitle(), lootViewerGetHubLines(),
                                lootViewerGetHubCount(), s_resultIndex,
                                "A/B Browse | Hold A Open | Hold B Back");
            break;
        case NAV_LOOT_FILE_LIST:
            uiRenderShowSubmenu(lootViewerGetTitle(), lootViewerGetFileListLines(),
                                lootViewerGetFileListCount(), s_resultIndex,
                                "A/B Browse | Hold A View | Hold B Back");
            break;
        case NAV_LOOT_FILE_VIEW:
            uiRenderShowSubmenu(lootViewerGetTitle(), lootViewerGetFileViewLines(),
                                lootViewerGetFileViewCount(), s_resultIndex,
                                "A/B Scroll | Hold B Back");
            break;
        case NAV_FLOCK_YOU_RUNNING: {
            char hdr[32];
            snprintf(hdr, sizeof(hdr), "Flock C:%d R:%d",
                     flockYouGetCamCount(), flockYouGetRavenCount());
            uiRenderShowSubmenu(hdr, flockYouGetResultLines(),
                                flockYouGetResultCount(), s_resultIndex,
                                "A/B Browse | LIVE | Hold B Back");
            break;
        }
        case NAV_DEAUTHER_SCAN_RUNNING:
            showWifiFeatureStatus("DEAUTHER", "Scanning targets...", "Hold B Cancel");
            break;
        case NAV_DEAUTHER_AP_LIST:
            uiRenderShowWifiList("DEAUTHER", deautherGetApResultLines(),
                                 deautherGetApResultCount(), s_resultIndex,
                                 "Hold A Start | Hold B Back");
            break;
        case NAV_DEAUTHER_ATTACK_RUNNING:
            showWifiFeatureStatus("DEAUTHER", "Starting attack...", "B-hold=Stop / Rescan");
            break;
        case NAV_WIFI_GUARDIAN_RUNNING:
            showFeatureStatus("WiFi Guardian", "starting...");
            break;
        case NAV_PROBE_SNIFFER_RUNNING:
            showWifiFeatureStatus("PROBE SNIFF", "Listening for probes...", "B-hold=Back");
            break;
        case NAV_PROBE_SNIFFER_RESULTS:
            uiRenderShowWifiList("PROBE SNIFF", probeSniffGetResultLines(),
                                 probeSniffGetResultCount(), s_resultIndex,
                                 "A/B Browse | LIVE | B-hold Back");
            break;
        case NAV_BEACON_SPAMMER_RUNNING:
            showWifiFeatureStatus("BEACON SPAM", "Starting transmitter...",
                                  "A/B CH | A-hold Nuke | B-hold Stop");
            break;
        case NAV_AUTH_FLOOD_SCAN_RUNNING:
            showWifiFeatureStatus("AUTHFLOOD", "Scanning targets...", "Hold B Cancel");
            break;
        case NAV_AUTH_FLOOD_AP_LIST:
            uiRenderShowWifiList("AUTHFLOOD", authFloodGetApResultLines(),
                                 authFloodGetApResultCount(), s_resultIndex,
                                 "Hold A Start | Hold B Back");
            break;
        case NAV_AUTH_FLOOD_ATTACK_RUNNING:
            showWifiFeatureStatus("AUTHFLOOD", "Starting attack...", "B-hold=Stop / Rescan");
            break;
        case NAV_STATION_SCAN_RUNNING:
            showWifiFeatureStatus("STATION SCANNER", "Listening for clients...", "B-hold=Back");
            break;
        case NAV_STATION_SCAN_RESULTS:
            uiRenderShowWifiList("STATIONS", stationScanGetResultLines(),
                                 stationScanGetResultCount(), s_resultIndex,
                                 "A/B Browse | LIVE | B-hold Back");
            break;
        case NAV_CAPTIVE_PORTAL_READY: {
            char readyBuf[RENDER_MSG_TEXT_MAX];
            snprintf(readyBuf, sizeof(readyBuf), "%s [%s]",
                     captivePortalGetSSID(), captivePortalGetTemplateName());
            showWifiFeatureStatus("EVIL TWIN", readyBuf,
                                  "A/B Template | A-hold Start | chord SSID");
            break;
        }
        case NAV_CAPTIVE_PORTAL_EDIT_SSID:
            showWifiFeatureStatus("EDIT SSID", captivePortalGetEditDisplay(),
                                  "A/B Char | A-hold Next | chord Save");
            break;
        case NAV_CAPTIVE_PORTAL_STARTING:
            showWifiFeatureStatus("EVIL TWIN", "Starting evil twin...", "Hold B Cancel");
            break;
        case NAV_CAPTIVE_PORTAL_ACTIVE:
            uiRenderShowWifiList("CREDENTIALS", captivePortalGetLogLines(),
                                 captivePortalGetLogCount(), s_resultIndex,
                                 "A/B Browse | LIVE | B-hold Stop");
            break;
        case NAV_SETTINGS_BRIGHTNESS: {
            char b[40];
            snprintf(b, sizeof(b), "Level %d (%d%%)",
                     g_settings.brightness, g_settings.brightness * 100 / 255);
            showFeatureStatus("Brightness", b);
            break;
        }
        case NAV_SETTINGS_TIMEOUT: {
            char b[40];
            snprintf(b, sizeof(b), "%s", SETTINGS_TIMEOUT_LABELS[s_timeoutIndex]);
            showFeatureStatus("Screen Timeout", b);
            break;
        }
        case NAV_SETTINGS_SWAP:
            showFeatureStatus("Swap Colors", g_settings.colorSwap ? "RGB" : "BGR (default)");
            break;
        case NAV_SETTINGS_INVERT:
            showFeatureStatus("Invert Display", g_settings.displayInverted ? "On" : "Off");
            break;
        case NAV_SETTINGS_COLOR_MODE:
            showFeatureStatus("Color Mode", SETTINGS_COLOR_MODE_LABELS[s_colorModeIndex]);
            break;
        case NAV_SETTINGS_ROTATION:
            showFeatureStatus("Rotation", SETTINGS_ROTATION_LABELS[s_rotationIndex]);
            break;
        case NAV_SETTINGS_DEVICE_INFO: {
            static char infoLines[8][40];
            static const char *infoPtrs[8];
            int pct = batteryGaugePercent();
            int mv = batteryGaugeMillivolts();
            snprintf(infoLines[0], sizeof(infoLines[0]), "Board: StickS3");
            snprintf(infoLines[1], sizeof(infoLines[1]), "Firmware: %s", FW_DEVICE);
            snprintf(infoLines[2], sizeof(infoLines[2]), "Edition: %s", FW_EDITION);
            snprintf(infoLines[3], sizeof(infoLines[3]), "Chip: ESP32-S3 @240MHz");
            snprintf(infoLines[4], sizeof(infoLines[4]), "Heap: %u KB  PSRAM: %u KB",
                     (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getPsramSize() / 1024));
            if (pct >= 0) {
                snprintf(infoLines[5], sizeof(infoLines[5]), "Battery: %d%%  %d mV",
                         pct, mv);
            } else {
                snprintf(infoLines[5], sizeof(infoLines[5]), "Battery: n/a");
            }
            snprintf(infoLines[6], sizeof(infoLines[6]), "Settings: NVS Storage: %s",
                     storageIsCardPresent() ? "LittleFS" : "unavailable");
            snprintf(infoLines[7], sizeof(infoLines[7]), "Drops I:%lu C:%lu L:%lu",
                     (unsigned long)droppedInputEvents, (unsigned long)droppedCaptureMsgs,
                     (unsigned long)droppedLogMsgs);
            for (int i = 0; i < 8; i++) infoPtrs[i] = infoLines[i];
            uiRenderShowInfo("Device Info", infoPtrs, 8);
            break;
        }
        case NAV_SETTINGS_PIN: {
            char b[40];
            snprintf(b, sizeof(b), "PIN: %s", g_settings.pinEnabled ? "On" : "Off");
            showFeatureStatus("Set PIN", b);
            break;
        }
        case NAV_ABOUT: {
            static char aboutLines[7][40];
            static const char *aboutPtrs[7];
            snprintf(aboutLines[0], sizeof(aboutLines[0]), "HaleHound %s", FW_EDITION);
            snprintf(aboutLines[1], sizeof(aboutLines[1]), "Board: M5Stack StickS3");
            snprintf(aboutLines[2], sizeof(aboutLines[2]), "Radios: WiFi / BLE (on-chip)");
            snprintf(aboutLines[3], sizeof(aboutLines[3]), "Input: A/B gesture buttons");
            snprintf(aboutLines[4], sizeof(aboutLines[4]), "Display: ST7789P3 240x135 IPS");
            snprintf(aboutLines[5], sizeof(aboutLines[5]), "Flash: 8MB   PSRAM: 8MB octal");
            snprintf(aboutLines[6], sizeof(aboutLines[6]), "HaleHound by Jesse C. Hale");
            for (int i = 0; i < 7; i++) aboutPtrs[i] = aboutLines[i];
            uiRenderShowInfo("HaleHound", aboutPtrs, 7);
            break;
        }
        case NAV_LOCKED:
        case NAV_PIN_ENTRY:
            uiRenderShowLock(s_pinMask);
            break;
    }
}

// Move the current level's selection by `dir` (+1 = down, -1 = up),
// wrapping around at the ends.
static void moveSelection(int dir) {
    if (s_navState == NAV_MAIN) {
        s_mainIndex = (s_mainIndex + dir + CARDPUTER_NUM_MENU_ITEMS) % CARDPUTER_NUM_MENU_ITEMS;
    } else if (s_navState == NAV_SUBMENU) {
        int count = CARDPUTER_MAIN_MENU[s_mainIndex].count;
        s_subIndex = (s_subIndex + dir + count) % count;
    } else if (s_navState == NAV_WIFI_SCAN_RESULTS) {
        int count = wifiScanGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_BLE_SCAN_RESULTS) {
        int count = bleScanGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_BLE_SKELETONKEY_RESULTS) {
        int count = skeletonKeyGetRowCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_BLE_SKELETONKEY_CONTROL) {
        s_resultIndex = (s_resultIndex + dir + SK_CONTROL_ITEMS) % SK_CONTROL_ITEMS;
    } else if (s_navState == NAV_BLE_SPOOFER_MENU) {
        int count = bleSpooferModeCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_LUNATIC_RESULTS) {
        int count = lunaticFringeGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_AIRTAG_RESULTS) {
        int count = airtagTrackerGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_EAPOL_AP_LIST) {
        int count = eapolSniffGetApResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_KARMA_RUNNING) {
        int count = karmaGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_SAVED_CAPTURES_LIST) {
        int count = savedCapturesGetFileListCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_SAVED_CAPTURES_VIEW) {
        int count = savedCapturesGetFileViewCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_IOT_RECON_RUNNING) {
        int count = iotReconGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_LOOT_HUB) {
        int count = lootViewerGetHubCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_LOOT_FILE_LIST) {
        int count = lootViewerGetFileListCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_LOOT_FILE_VIEW) {
        int count = lootViewerGetFileViewCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_FLOCK_YOU_RUNNING) {
        int count = flockYouGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_DEAUTHER_AP_LIST) {
        int count = deautherGetApResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_PROBE_SNIFFER_RESULTS) {
        int count = probeSniffGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_AUTH_FLOOD_AP_LIST) {
        int count = authFloodGetApResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_STATION_SCAN_RESULTS) {
        int count = stationScanGetResultCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_READY) {
        captivePortalAdjustTemplate(dir);
    } else if (s_navState == NAV_CAPTIVE_PORTAL_EDIT_SSID) {
        captivePortalEditRotate(dir);
    } else if (s_navState == NAV_CAPTIVE_PORTAL_ACTIVE) {
        int count = captivePortalGetLogCount();
        if (count > 0) s_resultIndex = (s_resultIndex + dir + count) % count;
    } else if (s_navState == NAV_PACKET_MONITOR_RUNNING) {
        packetMonitorAdjustChannel(dir);
    } else if (s_navState == NAV_BLE_SPOOFER_RUNNING) {
        bleSpooferAdjustMode(dir);
    } else if (s_navState == NAV_BLE_BEACON_RUNNING) {
        bleBeaconAdjustMode(dir);
    } else if (s_navState == NAV_BEACON_SPAMMER_RUNNING) {
        beaconSpammerAdjustChannel(dir);
    } else if (s_navState == NAV_SETTINGS_BRIGHTNESS) {
        int b = (int)g_settings.brightness + dir * 16;
        if (b < 16) b = 16;
        if (b > 255) b = 255;
        g_settings.brightness = (uint8_t)b;
        uiRenderSetBrightness(g_settings.brightness);
    } else if (s_navState == NAV_SETTINGS_TIMEOUT) {
        s_timeoutIndex = (s_timeoutIndex + dir + SETTINGS_TIMEOUT_COUNT) % SETTINGS_TIMEOUT_COUNT;
        g_settings.screenTimeout = (uint16_t)SETTINGS_TIMEOUT_VALUES[s_timeoutIndex];
    } else if (s_navState == NAV_SETTINGS_COLOR_MODE) {
        s_colorModeIndex = (s_colorModeIndex + dir + SETTINGS_COLOR_MODE_COUNT) % SETTINGS_COLOR_MODE_COUNT;
        g_settings.colorMode = (uint8_t)s_colorModeIndex;
        uiRenderApplySettings(g_settings.brightness, g_settings.rotationIndex,
                              g_settings.colorSwap, g_settings.displayInverted,
                              g_settings.colorMode);
    } else if (s_navState == NAV_SETTINGS_ROTATION) {
        s_rotationIndex = (s_rotationIndex + dir + SETTINGS_ROTATION_COUNT) % SETTINGS_ROTATION_COUNT;
        g_settings.rotationIndex = (uint8_t)s_rotationIndex;
        uiRenderApplySettings(g_settings.brightness, g_settings.rotationIndex,
                              g_settings.colorSwap, g_settings.displayInverted,
                              g_settings.colorMode);
    } else if (s_navState == NAV_PIN_ENTRY || s_navState == NAV_LOCKED) {
        int d = s_pinDigits[s_pinPos] + dir;
        if (d < 0) d = 9;
        if (d > 9) d = 0;
        s_pinDigits[s_pinPos] = (uint8_t)d;
        updatePinMask();
    }
    // Rotating while any other placeholder/running feature is up does nothing.
}

static void handleSelect() {
    if (s_navState == NAV_MAIN) {
        if (s_mainIndex == CARDPUTER_ABOUT_MENU_INDEX) {
            s_navState = NAV_ABOUT;
        } else {
            s_navState = NAV_SUBMENU;
            s_subIndex = 0;
        }
    } else if (s_navState == NAV_SUBMENU) {
        bool isBackRow = (s_subIndex == CARDPUTER_MAIN_MENU[s_mainIndex].count - 1);
        if (isBackRow) {
            s_navState = NAV_MAIN;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_PACKET_MONITOR) {
            packetMonitorStart();
            s_navState = NAV_PACKET_MONITOR_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_SCANNER) {
            wifiScanStart();
            s_navState = NAV_WIFI_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_BT_MENU_INDEX && s_subIndex == CARDPUTER_BT_SUBMENU_BLE_SPOOFER) {
            // Show the payload picker instead of auto-launching; cursor starts
            // on whatever mode was last selected this session.
            s_resultIndex = bleSpooferGetMode();
            s_navState = NAV_BLE_SPOOFER_MENU;
        } else if (s_mainIndex == CARDPUTER_BT_MENU_INDEX && s_subIndex == CARDPUTER_BT_SUBMENU_BLE_BEACON) {
            bleBeaconStart();
            s_navState = NAV_BLE_BEACON_RUNNING;
        } else if (s_mainIndex == CARDPUTER_BT_MENU_INDEX && s_subIndex == CARDPUTER_BT_SUBMENU_BLE_PREDATOR) {
            bleScanStart();
            s_navState = NAV_BLE_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_BT_MENU_INDEX && s_subIndex == CARDPUTER_BT_SUBMENU_LUNATIC_FRINGE) {
            lunaticFringeStart();
            s_navState = NAV_LUNATIC_RUNNING;
        } else if (s_mainIndex == CARDPUTER_BT_MENU_INDEX && s_subIndex == CARDPUTER_BT_SUBMENU_AIRTAG_TRACKER) {
            airtagTrackerStart();
            s_navState = NAV_AIRTAG_RUNNING;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_EAPOL_CAPTURE) {
            eapolSniffScanStart();
            s_navState = NAV_EAPOL_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_KARMA) {
            karmaStart();
            s_resultIndex = 0;
            s_navState = NAV_KARMA_RUNNING;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_WARDRIVING) {
            wardriveLiteStart();
            s_navState = NAV_WARDRIVE_LITE_RUNNING;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_SAVED_CAPTURES) {
            savedCapturesStart();
            s_resultIndex = 0;
            s_navState = NAV_SAVED_CAPTURES_BUSY;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_IOT_RECON) {
            iotReconStart();
            s_resultIndex = 0;
            s_navState = NAV_IOT_RECON_RUNNING;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_LOOT) {
            lootViewerStart();
            s_resultIndex = 0;
            s_navState = NAV_LOOT_BUSY;
        } else if (s_mainIndex == CARDPUTER_SIGINT_MENU_INDEX && s_subIndex == CARDPUTER_SIGINT_SUBMENU_FLOCK_YOU) {
            flockYouStart();
            s_resultIndex = 0;
            s_navState = NAV_FLOCK_YOU_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_DEAUTHER) {
            deautherScanStart();
            s_navState = NAV_DEAUTHER_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_JAMDETECT_MENU_INDEX && s_subIndex == CARDPUTER_JAMDETECT_SUBMENU_WIFI_GUARDIAN) {
            wifiGuardianStart();
            s_navState = NAV_WIFI_GUARDIAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_PROBE_SNIFFER) {
            probeSniffStart();
            s_navState = NAV_PROBE_SNIFFER_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_BEACON_SPAMMER) {
            beaconSpammerStart();
            s_navState = NAV_BEACON_SPAMMER_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_AUTH_FLOOD) {
            authFloodScanStart();
            s_navState = NAV_AUTH_FLOOD_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_STATION_SCAN) {
            stationScanStart();
            s_navState = NAV_STATION_SCAN_RUNNING;
        } else if (s_mainIndex == CARDPUTER_WIFI_MENU_INDEX && s_subIndex == WIFI_SUBMENU_CAPTIVE_PORTAL) {
            s_portalFromKarma = false;
            captivePortalReadyEnter();
            s_navState = NAV_CAPTIVE_PORTAL_READY;
        } else if (s_mainIndex == CARDPUTER_SETTINGS_MENU_INDEX) {
            switch (s_subIndex) {
                case SETTINGS_SUBMENU_BRIGHTNESS:
                    s_navState = NAV_SETTINGS_BRIGHTNESS;
                    break;
                case SETTINGS_SUBMENU_TIMEOUT:
                    for (int i = 0; i < SETTINGS_TIMEOUT_COUNT; i++) {
                        if (SETTINGS_TIMEOUT_VALUES[i] == (int)g_settings.screenTimeout) s_timeoutIndex = i;
                    }
                    s_navState = NAV_SETTINGS_TIMEOUT;
                    break;
                case SETTINGS_SUBMENU_SWAP:
                    s_navState = NAV_SETTINGS_SWAP;
                    break;
                case SETTINGS_SUBMENU_INVERT:
                    s_navState = NAV_SETTINGS_INVERT;
                    break;
                case SETTINGS_SUBMENU_COLOR_MODE:
                    s_colorModeIndex = g_settings.colorMode;
                    s_navState = NAV_SETTINGS_COLOR_MODE;
                    break;
                case SETTINGS_SUBMENU_ROTATION:
                    s_rotationIndex = g_settings.rotationIndex;
                    s_navState = NAV_SETTINGS_ROTATION;
                    break;
                case SETTINGS_SUBMENU_DEVICE_INFO:
                    s_navState = NAV_SETTINGS_DEVICE_INFO;
                    break;
                case SETTINGS_SUBMENU_PIN:
                    s_navState = NAV_SETTINGS_PIN;
                    break;
                default:
                    s_navState = NAV_PLACEHOLDER;
                    break;
            }
        } else {
            s_navState = NAV_PLACEHOLDER;
        }
    } else if (s_navState == NAV_BLE_SPOOFER_MENU) {
        // Commit the highlighted payload and start spamming it.
        bleSpooferSetMode(s_resultIndex);
        bleSpooferStart();
        s_navState = NAV_BLE_SPOOFER_RUNNING;
    } else if (s_navState == NAV_EAPOL_AP_LIST) {
        if (eapolSniffGetApResultCount() > 0 && eapolSniffApHasEapol(s_resultIndex)) {
            eapolSniffScanStop();
            eapolSniffSelectTarget(s_resultIndex);
            s_navState = NAV_EAPOL_CAPTURE_RUNNING;
        }
    } else if (s_navState == NAV_EAPOL_CAPTURE_RUNNING) {
        eapolSniffToggleDeauth();
    } else if (s_navState == NAV_KARMA_RUNNING) {
        const char *target = karmaGetSsid(s_resultIndex);
        if (target) {
            karmaStop();
            captivePortalSetSSID(target);
            s_portalFromKarma = true;
            captivePortalReadyEnter();
            s_navState = NAV_CAPTIVE_PORTAL_READY;
        }
    } else if (s_navState == NAV_SAVED_CAPTURES_LIST) {
        if (savedCapturesHasFiles()) {
            savedCapturesSelectFile(s_resultIndex);
            s_resultIndex = 0;
            s_navState = NAV_SAVED_CAPTURES_BUSY;
        }
    } else if (s_navState == NAV_LOOT_HUB) {
        lootViewerSelectCategory(s_resultIndex);
        s_resultIndex = 0;
        s_navState = NAV_LOOT_BUSY;
    } else if (s_navState == NAV_LOOT_FILE_LIST) {
        if (lootViewerHasFiles()) {
            lootViewerSelectFile(s_resultIndex);
            s_resultIndex = 0;
            s_navState = NAV_LOOT_BUSY;
        }
    } else if (s_navState == NAV_DEAUTHER_AP_LIST) {
        const char *const *rows = deautherGetApResultLines();
        if (deautherGetApResultCount() > 0 && rows && rows[s_resultIndex] &&
            strcmp(rows[s_resultIndex], "(no APs found)") != 0) {
            deautherScanStop();
            deautherSelectTarget(s_resultIndex);
            s_navState = NAV_DEAUTHER_ATTACK_RUNNING;
        }
    } else if (s_navState == NAV_WIFI_SCAN_RESULTS &&
               wifiScanGetResultCount() > 0 && wifiScanGetResultChannel(s_resultIndex) != 0) {
        // Tap-to-attack handoff, ported from wifi_attacks.cpp's WifiScan:
        // selecting a network from Scanner results jumps straight into
        // Deauther against it, skipping Deauther's own scan phase.
        const uint8_t *bssid = wifiScanGetResultBSSID(s_resultIndex);
        const char *ssid = wifiScanGetResultSSID(s_resultIndex);
        uint8_t channel = wifiScanGetResultChannel(s_resultIndex);
        wifiScanStop();
        deautherSetTarget(bssid, ssid, channel);
        s_navState = NAV_DEAUTHER_ATTACK_RUNNING;
    } else if (s_navState == NAV_BLE_SCAN_RESULTS) {
        // Predator-to-SkeletonKey handoff — selecting a device from
        // Predator's live list jumps straight into a GATT connect/walk.
        uint8_t mac[6], addrType;
        if (bleScanGetTargetMac(s_resultIndex, mac, &addrType)) {
            char devName[32] = {0};
            bleScanGetTargetName(s_resultIndex, devName, sizeof(devName));
            bleScanStop();
            skeletonKeyStart(mac, addrType, devName[0] ? devName : nullptr);
            s_navState = NAV_BLE_SKELETONKEY_CONNECTING;
        }
    } else if (s_navState == NAV_BLE_SKELETONKEY_RESULTS) {
        skeletonKeyRequestRead(s_resultIndex);
    } else if (s_navState == NAV_BLE_SKELETONKEY_CONTROL) {
        using namespace SkeletonKeyProtocol;
        switch (s_resultIndex) {
            case 0: skeletonKeyRequestControl(PowerOn); break;
            case 1: skeletonKeyRequestControl(PowerOff); break;
            case 2: skeletonKeyRequestControl(Red); break;
            case 3: skeletonKeyRequestControl(Green); break;
            case 4: skeletonKeyRequestControl(Blue); break;
            case 5: skeletonKeyRequestControl(White); break;
            case 6: {
                s_skColorIndex = (s_skColorIndex + 1) % SK_COLOR_MIX_COUNT;
                const SkColorPreset &c = SK_COLOR_MIX[s_skColorIndex];
                skeletonKeyRequestControl(Color, c.r, c.g, c.b);
                break;
            }
            case 7:
                s_skBrightness = s_skBrightness >= 10 ? s_skBrightness - 10 : 0;
                skeletonKeyRequestControl(Brightness, 0, 0, 0, uint8_t(s_skBrightness));
                break;
            case 8:
                s_skBrightness = s_skBrightness <= 90 ? s_skBrightness + 10 : 100;
                skeletonKeyRequestControl(Brightness, 0, 0, 0, uint8_t(s_skBrightness));
                break;
            case 9: {
                skeletonKeyGetSnapshot(&s_skSnapshot);
                int next = skNextCompatibleProfile(s_skSnapshot.compatibleProfiles, s_skSnapshot.profileIndex);
                if (next >= 0) skeletonKeyRequestProfile(next);
                break;
            }
            case 10:
                s_navState = NAV_BLE_SKELETONKEY_RESULTS;
                s_resultIndex = 0;
                break;
            case 11:
                skeletonKeyRequestRescan();
                break;
        }
    } else if (s_navState == NAV_AUTH_FLOOD_AP_LIST) {
        const char *const *rows = authFloodGetApResultLines();
        if (authFloodGetApResultCount() > 0 && rows && rows[s_resultIndex] &&
            strcmp(rows[s_resultIndex], "(no APs found)") != 0) {
            authFloodScanStop();
            authFloodSelectTarget(s_resultIndex);
            s_navState = NAV_AUTH_FLOOD_ATTACK_RUNNING;
        }
    } else if (s_navState == NAV_CAPTIVE_PORTAL_READY) {
        captivePortalStart();
        s_navState = NAV_CAPTIVE_PORTAL_STARTING;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_EDIT_SSID) {
        captivePortalEditCommitChar();
    } else if (s_navState == NAV_BEACON_SPAMMER_RUNNING) {
        beaconSpammerToggleNuke();
    } else if (s_navState == NAV_SETTINGS_SWAP) {
        g_settings.colorSwap = !g_settings.colorSwap;
        uiRenderApplySettings(g_settings.brightness, g_settings.rotationIndex,
                              g_settings.colorSwap, g_settings.displayInverted,
                              g_settings.colorMode);
    } else if (s_navState == NAV_SETTINGS_INVERT) {
        g_settings.displayInverted = !g_settings.displayInverted;
        uiRenderApplySettings(g_settings.brightness, g_settings.rotationIndex,
                              g_settings.colorSwap, g_settings.displayInverted,
                              g_settings.colorMode);
    } else if (s_navState == NAV_SETTINGS_PIN) {
        memset(s_pinDigits, 0, sizeof(s_pinDigits));
        s_pinPos = 0;
        updatePinMask();
        s_navState = NAV_PIN_ENTRY;
    } else if (s_navState == NAV_PIN_ENTRY) {
        s_pinPos++;
        if (s_pinPos >= 4) {
            g_settings.devicePin = pinFromDigits();
            g_settings.pinEnabled = 1;
            settingsSave();
            s_pinPos = 0;
            s_navState = NAV_SETTINGS_PIN;
        } else {
            updatePinMask();
        }
    } else if (s_navState == NAV_LOCKED) {
        s_pinPos++;
        if (s_pinPos >= 4) {
            if (pinFromDigits() == g_settings.devicePin) {
                s_navState = NAV_MAIN;
                s_mainIndex = 0;
            } else {
                memset(s_pinDigits, 0, sizeof(s_pinDigits));
                s_pinPos = 0;
                updatePinMask();
            }
        } else {
            updatePinMask();
        }
    }
}

static void handleBack() {
    if (s_navState == NAV_PLACEHOLDER) {
        s_navState = NAV_SUBMENU;
    } else if (s_navState == NAV_WIFI_SCAN_RUNNING || s_navState == NAV_WIFI_SCAN_RESULTS) {
        wifiScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_SCANNER;
    } else if (s_navState == NAV_PACKET_MONITOR_RUNNING) {
        packetMonitorStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_PACKET_MONITOR;
    } else if (s_navState == NAV_BLE_SKELETONKEY_CONNECTING || s_navState == NAV_BLE_SKELETONKEY_RESULTS ||
               s_navState == NAV_BLE_SKELETONKEY_CONTROL) {
        skeletonKeyStop();
        bleScanStart();
        s_navState = NAV_BLE_SCAN_RUNNING;
    } else if (s_navState == NAV_BLE_SCAN_RUNNING || s_navState == NAV_BLE_SCAN_RESULTS) {
        bleScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_BT_SUBMENU_BLE_PREDATOR;
    } else if (s_navState == NAV_BLE_SPOOFER_MENU) {
        // Back out of the payload picker to the Bluetooth submenu.
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_BT_SUBMENU_BLE_SPOOFER;
    } else if (s_navState == NAV_BLE_SPOOFER_RUNNING) {
        // Back from a running payload returns to the picker (not the submenu),
        // so the user can pick another without re-entering the menu.
        bleSpooferStop();
        s_resultIndex = bleSpooferGetMode();
        s_navState = NAV_BLE_SPOOFER_MENU;
    } else if (s_navState == NAV_BLE_BEACON_RUNNING) {
        bleBeaconStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_BT_SUBMENU_BLE_BEACON;
    } else if (s_navState == NAV_LUNATIC_RUNNING || s_navState == NAV_LUNATIC_RESULTS) {
        lunaticFringeStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_BT_SUBMENU_LUNATIC_FRINGE;
    } else if (s_navState == NAV_AIRTAG_RUNNING || s_navState == NAV_AIRTAG_RESULTS) {
        airtagTrackerStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_BT_SUBMENU_AIRTAG_TRACKER;
    } else if (s_navState == NAV_EAPOL_SCAN_RUNNING || s_navState == NAV_EAPOL_AP_LIST) {
        eapolSniffScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_EAPOL_CAPTURE;
    } else if (s_navState == NAV_EAPOL_CAPTURE_RUNNING) {
        eapolSniffCaptureStop();
        eapolSniffScanStart();
        s_navState = NAV_EAPOL_SCAN_RUNNING;
    } else if (s_navState == NAV_WARDRIVE_LITE_RUNNING) {
        wardriveLiteStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_WARDRIVING;
    } else if (s_navState == NAV_KARMA_RUNNING) {
        karmaStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_KARMA;
    } else if (s_navState == NAV_SAVED_CAPTURES_BUSY || s_navState == NAV_SAVED_CAPTURES_LIST) {
        savedCapturesStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_SAVED_CAPTURES;
    } else if (s_navState == NAV_SAVED_CAPTURES_VIEW) {
        savedCapturesBack();
        s_navState = NAV_SAVED_CAPTURES_LIST;
        s_resultIndex = 0;
    } else if (s_navState == NAV_IOT_RECON_RUNNING) {
        iotReconStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_IOT_RECON;
    } else if (s_navState == NAV_LOOT_BUSY || s_navState == NAV_LOOT_HUB) {
        lootViewerStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_LOOT;
    } else if (s_navState == NAV_LOOT_FILE_LIST) {
        lootViewerBack();
        s_navState = NAV_LOOT_HUB;
        s_resultIndex = 0;
    } else if (s_navState == NAV_LOOT_FILE_VIEW) {
        lootViewerBack();
        s_navState = NAV_LOOT_FILE_LIST;
        s_resultIndex = 0;
    } else if (s_navState == NAV_FLOCK_YOU_RUNNING) {
        flockYouStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_SIGINT_SUBMENU_FLOCK_YOU;
    } else if (s_navState == NAV_DEAUTHER_SCAN_RUNNING || s_navState == NAV_DEAUTHER_AP_LIST) {
        deautherScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_DEAUTHER;
    } else if (s_navState == NAV_DEAUTHER_ATTACK_RUNNING) {
        deautherAttackStop();
        deautherScanStart();
        s_navState = NAV_DEAUTHER_SCAN_RUNNING;
    } else if (s_navState == NAV_WIFI_GUARDIAN_RUNNING) {
        wifiGuardianStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = CARDPUTER_JAMDETECT_SUBMENU_WIFI_GUARDIAN;
    } else if (s_navState == NAV_PROBE_SNIFFER_RUNNING || s_navState == NAV_PROBE_SNIFFER_RESULTS) {
        probeSniffStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_PROBE_SNIFFER;
    } else if (s_navState == NAV_BEACON_SPAMMER_RUNNING) {
        beaconSpammerStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_BEACON_SPAMMER;
    } else if (s_navState == NAV_AUTH_FLOOD_SCAN_RUNNING || s_navState == NAV_AUTH_FLOOD_AP_LIST) {
        authFloodScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_AUTH_FLOOD;
    } else if (s_navState == NAV_AUTH_FLOOD_ATTACK_RUNNING) {
        authFloodAttackStop();
        authFloodScanStart();
        s_navState = NAV_AUTH_FLOOD_SCAN_RUNNING;
    } else if (s_navState == NAV_STATION_SCAN_RUNNING || s_navState == NAV_STATION_SCAN_RESULTS) {
        stationScanStop();
        s_navState = NAV_SUBMENU;
        s_subIndex = WIFI_SUBMENU_STATION_SCAN;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_READY) {
        s_navState = NAV_SUBMENU;
        s_mainIndex = s_portalFromKarma ? CARDPUTER_SIGINT_MENU_INDEX : CARDPUTER_WIFI_MENU_INDEX;
        s_subIndex = s_portalFromKarma ? CARDPUTER_SIGINT_SUBMENU_KARMA : WIFI_SUBMENU_CAPTIVE_PORTAL;
        s_portalFromKarma = false;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_EDIT_SSID) {
        captivePortalEditCancel();
        s_navState = NAV_CAPTIVE_PORTAL_READY;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_STARTING || s_navState == NAV_CAPTIVE_PORTAL_ACTIVE) {
        captivePortalStop();
        s_navState = NAV_CAPTIVE_PORTAL_READY;
    } else if (s_navState == NAV_SETTINGS_BRIGHTNESS) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_BRIGHTNESS;
    } else if (s_navState == NAV_SETTINGS_TIMEOUT) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_TIMEOUT;
    } else if (s_navState == NAV_SETTINGS_SWAP) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_SWAP;
    } else if (s_navState == NAV_SETTINGS_INVERT) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_INVERT;
    } else if (s_navState == NAV_SETTINGS_COLOR_MODE) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_COLOR_MODE;
    } else if (s_navState == NAV_SETTINGS_ROTATION) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_ROTATION;
    } else if (s_navState == NAV_SETTINGS_DEVICE_INFO) {
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_DEVICE_INFO;
    } else if (s_navState == NAV_SETTINGS_PIN) {
        settingsSave();
        s_navState = NAV_SUBMENU;
        s_subIndex = SETTINGS_SUBMENU_PIN;
    } else if (s_navState == NAV_PIN_ENTRY) {
        s_navState = NAV_SETTINGS_PIN;
    } else if (s_navState == NAV_ABOUT) {
        s_navState = NAV_MAIN;
    } else if (s_navState == NAV_LOCKED) {
        // Locked — BACK does nothing; the PIN is the only way out.
    } else if (s_navState == NAV_SUBMENU) {
        s_navState = NAV_MAIN;
    }
    // BACK at the main menu does nothing — nowhere higher to go.
}

static void handleLongPress() {
    if (s_navState == NAV_EAPOL_CAPTURE_RUNNING) {
        eapolSniffSaveNow();
    } else if (s_navState == NAV_SETTINGS_PIN) {
        g_settings.pinEnabled = 0;
        settingsSave();
    } else if (s_navState == NAV_BLE_SKELETONKEY_RESULTS) {
        // Jump straight to the light controls if this device matched any
        // known profile's GATT endpoint (even one the name-based detect()
        // didn't auto-select — see skNextCompatibleProfile()); otherwise
        // there is nothing to control, so fall back to the old rescan.
        skeletonKeyGetSnapshot(&s_skSnapshot);
        if (s_skSnapshot.compatibleProfiles) {
            s_navState = NAV_BLE_SKELETONKEY_CONTROL;
            s_resultIndex = 0;
        } else {
            skeletonKeyRequestRescan();
        }
    } else if (s_navState == NAV_BLE_SKELETONKEY_CONTROL) {
        skeletonKeyRequestRescan();
    } else if (s_navState == NAV_BLE_SCAN_RESULTS) {
        bleScanCycleFilter();
    } else if (s_navState == NAV_CAPTIVE_PORTAL_READY) {
        captivePortalEditSSIDBegin();
        s_navState = NAV_CAPTIVE_PORTAL_EDIT_SSID;
    } else if (s_navState == NAV_CAPTIVE_PORTAL_EDIT_SSID) {
        captivePortalEditFinish();
        s_navState = NAV_CAPTIVE_PORTAL_READY;
    }
    // Long-press does nothing in any other state so far.
}

void setup() {
    // M5PM1 handles power-on in hardware — no pin to assert first thing
    // here (unlike the T-Embed's BOARD_PWR_EN).
    Serial.begin(115200);
    uint32_t usbWaitStart = millis();
    while (!Serial && (millis() - usbWaitStart) < 3000) {
        delay(10);
    }

    Serial.println();
    Serial.println("=== " FW_DEVICE " " FW_EDITION " ===");
    Serial.println("Stages 1-4: board bring-up + WiFi/BLE features (not hardware-verified)");

    spiManagerSetup();
    radioManagerSetup();
    #if CYD_HAS_I2C_BUS
    i2cManagerSetup();
    #endif
    if (!boardPowerSetup()) {
        Serial.println("[SETUP] M5PM1 L3B power rail unavailable; display may stay dark");
    }
    messagingInit();
    settingsManagerSetup();
    batteryGaugeSetup();
    batteryGaugeStart();
    if (!psramPoolInit()) {
        Serial.println("[SETUP] WARNING: capture/log pool init failed");
    }
    if (!storageManagerSetup()) {
        Serial.println("[SETUP] LittleFS unavailable; capture files will not be saved");
    }

    #if CYD_HAS_BUTTONS
    buttonInputSetup();
    #endif

    uiRenderStart();
    // After uiRenderStart() — it creates the state mutex this takes.
    uiRenderApplySettings(g_settings.brightness, g_settings.rotationIndex,
                          g_settings.colorSwap, g_settings.displayInverted,
                          g_settings.colorMode);
    inputTaskStart();

    // Boot splash while the render/input tasks settle.
    uiRenderShowSplash();
    delay(1200);

    // Seed the settings-screen working copies + the idle timer.
    for (int i = 0; i < SETTINGS_TIMEOUT_COUNT; i++) {
        if (SETTINGS_TIMEOUT_VALUES[i] == (int)g_settings.screenTimeout) s_timeoutIndex = i;
    }
    s_colorModeIndex = g_settings.colorMode;
    s_rotationIndex = g_settings.rotationIndex;
    s_lastInputMs = millis();

    if (g_settings.pinEnabled) {
        memset(s_pinDigits, 0, sizeof(s_pinDigits));
        s_pinPos = 0;
        updatePinMask();
        s_navState = NAV_LOCKED;
    }
    showCurrentScreen();

    Serial.println("Menu ready. A=Next/Select(hold), B=Previous/Back(hold), A+B=secondary.");
}

static const char *eventName(InputEventType t) {
    switch (t) {
        case INPUT_EVENT_ENC_CW:    return "CW";
        case INPUT_EVENT_ENC_CCW:   return "CCW";
        case INPUT_EVENT_BTN_SHORT: return "SELECT";
        case INPUT_EVENT_BTN_LONG:  return "BTN_LONG";
        case INPUT_EVENT_BACK:      return "BACK";
        default:                    return "?";
    }
}

static void handleInputEvent(const InputEvent &ev) {
    s_lastInputMs = millis();

    if (s_screenBlanked) {
        s_screenBlanked = false;
        uiRenderSetBacklight(true);
        return;
    }

    Serial.printf("[NAV] event=%s state=%d main=%d sub=%d\n",
                  eventName(ev.type), (int)s_navState, s_mainIndex, s_subIndex);

    bool dirty = true;
    switch (ev.type) {
        case INPUT_EVENT_ENC_CW:
            moveSelection(+1);
            break;
        case INPUT_EVENT_ENC_CCW:
            moveSelection(-1);
            break;
        case INPUT_EVENT_BTN_SHORT:
            handleSelect();
            break;
        case INPUT_EVENT_BTN_LONG:
            handleLongPress();
            break;
        case INPUT_EVENT_BACK:
            handleBack();
            break;
        default:
            dirty = false;
            break;
    }
    if (dirty) {
        showCurrentScreen();
    }
}

void loop() {
    // Screen timeout — blank the backlight after the configured idle period.
    if (g_settings.screenTimeout > 0 && !s_screenBlanked &&
        (millis() - s_lastInputMs) > (uint32_t)g_settings.screenTimeout * 1000) {
        uiRenderSetBacklight(false);
        s_screenBlanked = true;
    }

    // Battery — re-draw only when the percentage actually changes. The
    // actual sampling (once implemented) happens on its own task
    // (battery_gauge.cpp's BOARD_M5STICK_S3 branch); this just checks the
    // cached value, which currently stays at -1 (unavailable).
    static int lastBatteryPct = -2;
    int batteryPct = batteryGaugePercent();
    if (batteryPct != lastBatteryPct) {
        lastBatteryPct = batteryPct;
        uiRenderRefresh();
    }

    if (isFeatureRunning()) {
        // Check telemetry first (short timeout), then input (non-blocking),
        // so a running worker's updates and a BACK press are both handled
        // promptly without spin-polling.
        RenderMsg msg;
        if (xQueueReceive(telemetry_q, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_WIFI_SCAN_RUNNING) {
                s_navState = NAV_WIFI_SCAN_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_BLE_SCAN_RUNNING) {
                s_navState = NAV_BLE_SCAN_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_BLE_SCAN_RESULTS) {
                int count = bleScanGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_BLE_SKELETONKEY_CONNECTING) {
                if (skeletonKeyGetState() == SK_STATE_CONNECTED) {
                    // Land on the light-control screen whenever this device
                    // matched a known profile's GATT endpoint, even if
                    // name-based detect() didn't pick one (manual profile
                    // selection is available from there); otherwise there's
                    // nothing to control, so fall back to the raw GATT list.
                    skeletonKeyGetSnapshot(&s_skSnapshot);
                    s_navState = s_skSnapshot.compatibleProfiles ? NAV_BLE_SKELETONKEY_CONTROL
                                                                  : NAV_BLE_SKELETONKEY_RESULTS;
                    s_resultIndex = 0;
                    s_skBrightness = 100;
                    s_skColorIndex = 0;
                }
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE &&
                       (s_navState == NAV_BLE_SKELETONKEY_RESULTS ||
                        s_navState == NAV_BLE_SKELETONKEY_CONTROL)) {
                if (skeletonKeyGetState() == SK_STATE_CONNECTING) {
                    s_navState = NAV_BLE_SKELETONKEY_CONNECTING;
                }
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_LUNATIC_RUNNING) {
                s_navState = NAV_LUNATIC_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_LUNATIC_RESULTS) {
                int count = lunaticFringeGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_AIRTAG_RUNNING) {
                s_navState = NAV_AIRTAG_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_AIRTAG_RESULTS) {
                int count = airtagTrackerGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_BLE_SPOOFER_RUNNING) {
                showFeatureStatus("BLE Spoofer", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_BLE_BEACON_RUNNING) {
                showFeatureStatus("BLE Beacon", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_WIFI_MONITOR && s_navState == NAV_PACKET_MONITOR_RUNNING) {
                uiRenderShowWifiMonitor(&msg.payload.wifiMonitor);
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_PACKET_MONITOR_RUNNING) {
                showWifiFeatureStatus("PKT MONITOR", msg.payload.text, "A/B Channel | B-hold Back");
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_EAPOL_SCAN_RUNNING) {
                s_navState = NAV_EAPOL_AP_LIST;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_EAPOL_CAPTURE_RUNNING) {
                showFeatureStatus("EAPOL Capture", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_WARDRIVE_LITE_RUNNING) {
                showFeatureStatus("Wardriving", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_KARMA_RUNNING) {
                int count = karmaGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_SAVED_CAPTURES_BUSY) {
                showFeatureStatus("Captures", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_SAVED_CAPTURES_BUSY) {
                SavedCapturesPhase phase = savedCapturesGetPhase();
                if (phase == SC_PHASE_FILE_LIST) {
                    s_navState = NAV_SAVED_CAPTURES_LIST;
                    s_resultIndex = 0;
                    showCurrentScreen();
                } else if (phase == SC_PHASE_FILE_VIEW) {
                    s_navState = NAV_SAVED_CAPTURES_VIEW;
                    s_resultIndex = 0;
                    showCurrentScreen();
                }
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_IOT_RECON_RUNNING) {
                int count = iotReconGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_LOOT_BUSY) {
                showFeatureStatus("Loot", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_LOOT_BUSY) {
                LootViewerPhase phase = lootViewerGetPhase();
                if (phase == LV_PHASE_HUB) {
                    s_navState = NAV_LOOT_HUB;
                } else if (phase == LV_PHASE_FILE_LIST) {
                    s_navState = NAV_LOOT_FILE_LIST;
                } else if (phase == LV_PHASE_FILE_VIEW) {
                    s_navState = NAV_LOOT_FILE_VIEW;
                }
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_FLOCK_YOU_RUNNING) {
                int count = flockYouGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_DEAUTHER_SCAN_RUNNING) {
                s_navState = NAV_DEAUTHER_AP_LIST;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_DEAUTHER_ATTACK_RUNNING) {
                showWifiFeatureStatus("DEAUTHER", msg.payload.text, "B-hold=Stop / Rescan");
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_WIFI_GUARDIAN_RUNNING) {
                showFeatureStatus("WiFi Guardian", msg.payload.text);
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_PROBE_SNIFFER_RUNNING) {
                s_navState = NAV_PROBE_SNIFFER_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_PROBE_SNIFFER_RESULTS) {
                int count = probeSniffGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_BEACON_SPAMMER_RUNNING) {
                showWifiFeatureStatus("BEACON SPAM", msg.payload.text,
                                      "A/B CH | A-hold Nuke | B-hold Stop");
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_AUTH_FLOOD_SCAN_RUNNING) {
                s_navState = NAV_AUTH_FLOOD_AP_LIST;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_STATUS_TEXT && s_navState == NAV_AUTH_FLOOD_ATTACK_RUNNING) {
                showWifiFeatureStatus("AUTHFLOOD", msg.payload.text, "B-hold=Stop / Rescan");
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_STATION_SCAN_RUNNING) {
                s_navState = NAV_STATION_SCAN_RESULTS;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_STATION_SCAN_RESULTS) {
                int count = stationScanGetResultCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_CAPTIVE_PORTAL_STARTING) {
                s_navState = NAV_CAPTIVE_PORTAL_ACTIVE;
                s_resultIndex = 0;
                showCurrentScreen();
            } else if (msg.kind == RENDER_MSG_SCAN_COMPLETE && s_navState == NAV_CAPTIVE_PORTAL_ACTIVE) {
                int count = captivePortalGetLogCount();
                if (s_resultIndex >= count) s_resultIndex = (count > 0) ? count - 1 : 0;
                showCurrentScreen();
            }
        }

        InputEvent ev;
        if (xQueueReceive(ui_input_q, &ev, 0) == pdTRUE) {
            handleInputEvent(ev);
        }
    } else {
        InputEvent ev;
        if (xQueueReceive(ui_input_q, &ev, pdMS_TO_TICKS(100)) == pdTRUE) {
            handleInputEvent(ev);
        }
    }
}
