// ═══════════════════════════════════════════════════════════════════════════
// HaleHound UI Render Task — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 3)
//
// NOTE: deliberately does NOT use the shared CYD_SCREEN_WIDTH/HEIGHT or
// SCALE_X/Y layout macros from cyd_config.h — those are the panel's NATIVE
// portrait dimensions (170x320), but this UI runs the panel rotated to
// landscape (320x170) at runtime. Using tft.width()/tft.height() (read
// AFTER setRotation()) avoids that mismatch; a CYD-style landscape layout
// header can be added later if T-Embed grows enough screens to want one.
// ═══════════════════════════════════════════════════════════════════════════

#include "ui_render.h"
#include <TFT_eSPI.h>
#include "cyd_config.h"
#include "spi_manager.h"
#include "menu_data.h"
#include "messaging.h"
#include "battery_gauge.h"
#include "settings_manager.h"
#include "icon.h"
#include "skull_bg.h"
#include "nosifer_font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <cstring>

// CYD build's palette (utils.cpp defaults) — used by the BLE Predator
// screen to match the original's colors exactly.
#define HH_BLUE     0x041F   // HALEHOUND_MAGENTA (Electric Blue)
#define HH_PINK     0xF81F   // HALEHOUND_HOTPINK / BRIGHT
#define HH_VIOLET   0x780F   // HALEHOUND_VIOLET
#define HH_DARK     0x2841   // HALEHOUND_DARK
#define HH_GUNMETAL 0x18E3   // HALEHOUND_GUNMETAL

static TFT_eSPI tft = TFT_eSPI();
static TFT_eSprite sprite = TFT_eSprite(&tft);
static TaskHandle_t s_taskHandle = NULL;
static SemaphoreHandle_t s_stateMutex = NULL;

struct ScreenState {
    enum Kind { MAIN_MENU, SUBMENU, PLACEHOLDER, STATUS, WIFI_STATUS, WIFI_LIST,
                WIFI_MONITOR, SPECTRUM, NRF_SCANNER,
                NRF_ANALYZER, NRF_SNIFFER, BLE_LIST, MOUSEJACK, SPLASH, INFO,
                LOCK } kind;
    char title[24];
    const char *const *items;
    const unsigned char *const *icons; // optional per-row 16x16 icons (submenus)
    int count;
    int selected;
    char message[RENDER_MSG_TEXT_MAX];
    char hint[48];
    uint8_t spectrum[RENDER_MSG_SPECTRUM_BINS];
    int specCount;
    int8_t lockWifiCh; // NRF analyzer AP lock (0 = full band)
    bool running;      // NRF analyzer start/stop state
};

static ScreenState s_state = {};
static const char *s_mainMenuTitles[NUM_MENU_ITEMS];

// Display settings — written by the orchestrator under s_stateMutex, applied
// by the render task (sole owner of tft/backlight) at the top of each wake.
struct DisplaySettings {
    uint8_t brightness;     // 0-255 LEDC duty
    uint8_t rotationIndex;  // 0 = Normal, 1 = Flipped
    bool swapColors;        // true = RGB order, false = BGR (panel default)
    bool invert;
    uint8_t colorMode;      // 0/1/2
    bool backlightOn;
};
static DisplaySettings s_disp = {255, 0, false, false, 0, true};

// Palette for the generic screens — mirrors CYD utils.cpp's color modes.
static uint16_t s_colTitle = TFT_MAGENTA;
static uint16_t s_colText = TFT_CYAN;
static uint16_t s_colHighlight = TFT_MAGENTA;
static uint16_t s_colHighlightText = TFT_BLACK;

// Applied-value tracking so MADCTL/invert/brightness writes only happen when
// something actually changed. s_appliedBrightness starts at 0 because
// ledcAttachPin() leaves the channel at duty 0 — the first apply must write
// the real duty or the backlight stays dark.
static uint8_t s_appliedRotation = 255;
static uint8_t s_appliedMadctl = 255;
static bool s_appliedInvert = false;
static bool s_appliedInvertValid = false;
static uint8_t s_appliedColorMode = 255;
static uint8_t s_appliedBrightness = 0;
static bool s_appliedBacklightOn = false;

static void applyPalette(uint8_t mode) {
    switch (mode) {
        case 1: // Colorblind — bright blue + yellow
            s_colTitle = 0x067F;
            s_colText = 0x067F;
            s_colHighlight = 0xFFE0;
            s_colHighlightText = TFT_BLACK;
            break;
        case 2: // High Contrast — bright cyan + white
            s_colTitle = 0x07FF;
            s_colText = 0x07FF;
            s_colHighlight = TFT_WHITE;
            s_colHighlightText = TFT_BLACK;
            break;
        default: // Default — magenta + cyan
            s_colTitle = TFT_MAGENTA;
            s_colText = TFT_CYAN;
            s_colHighlight = TFT_MAGENTA;
            s_colHighlightText = TFT_BLACK;
            break;
    }
}

static uint8_t madctlForRotation(uint8_t rot, bool swapColors) {
    uint8_t madctl;
    switch (rot) {
        case 0:  madctl = 0; break;
        case 1:  madctl = TFT_MAD_MX | TFT_MAD_MV; break;
        case 2:  madctl = TFT_MAD_MX | TFT_MAD_MY; break;
        default: madctl = TFT_MAD_MV | TFT_MAD_MY; break;
    }
    // TFT_eSPI's ST7789 init writes BGR order by default; swapColors flips it.
    madctl |= swapColors ? TFT_MAD_RGB : TFT_MAD_BGR;
    return madctl;
}

static void applyDisplaySettings(const DisplaySettings &d) {
    uint8_t tftRot = (d.rotationIndex == 1) ? 1 : 3;
    uint8_t madctl = madctlForRotation(tftRot, d.swapColors);
    bool panelChanged = s_appliedRotation != tftRot || s_appliedMadctl != madctl ||
                        !s_appliedInvertValid || s_appliedInvert != d.invert;

    // Settings issue real panel commands too; the SD worker may still be
    // draining writes while the user is in the settings menu.
    if (panelChanged && spiSelectDisplay()) {
        if (s_appliedRotation != tftRot) {
            tft.setRotation(tftRot);
            s_appliedRotation = tftRot;
            s_appliedMadctl = 255; // setRotation rewrote MADCTL — force re-apply
        }

        if (s_appliedMadctl != madctl) {
            tft.writecommand(TFT_MADCTL);
            tft.writedata(madctl);
            s_appliedMadctl = madctl;
        }

        if (!s_appliedInvertValid || s_appliedInvert != d.invert) {
            tft.invertDisplay(d.invert);
            s_appliedInvert = d.invert;
            s_appliedInvertValid = true;
        }
        spiDeselect();
    }

    if (s_appliedColorMode != d.colorMode) {
        applyPalette(d.colorMode);
        s_appliedColorMode = d.colorMode;
    }

    uint8_t duty = d.backlightOn ? d.brightness : 0;
    if (s_appliedBrightness != duty) {
        ledcWrite(0, duty);
        s_appliedBrightness = duty;
    }
    s_appliedBacklightOn = d.backlightOn;
}

// BLE Predator screen state — kept out of ScreenState so the render task's
// per-wake copy stays small; guarded by the same s_stateMutex. The render
// task snapshots rows into s_bleDrawRows before drawing.
static BleDisplayRow s_bleRows[UI_BLE_MAX_ROWS];
static BleDisplayStats s_bleStats;
static int s_bleSelected = 0;
static BleDisplayRow s_bleDrawRows[UI_BLE_MAX_ROWS];
static BleDisplayStats s_bleDrawStats;
static int s_bleDrawSelected = 0;

// NRF sniffer table — same snapshot-under-mutex shape as the BLE list above.
static NrfSniffDisplayRow s_nrfSniffRows[UI_NRF_MAX_ROWS];
static NrfSniffDisplayStats s_nrfSniffStats;
static int s_nrfSniffSelected = 0;
static NrfSniffDisplayRow s_nrfSniffDrawRows[UI_NRF_MAX_ROWS];
static NrfSniffDisplayStats s_nrfSniffDrawStats;
static int s_nrfSniffDrawSelected = 0;
static int s_nrfSniffFrame = 0;

// MouseJack screen — same snapshot shape.
static MouseJackDisplay s_mjDisplay;
static MouseJackDisplay s_mjDraw;

// WiFi packet-monitor snapshots and compact history waterfall.
static WifiMonitorMsg s_wifiMonitor = {};
static WifiMonitorMsg s_wifiMonitorDraw = {};
#define WIFI_WATERFALL_ROWS 96
static uint8_t s_wifiHistory[WIFI_WATERFALL_ROWS][WIFI_MONITOR_BINS] = {};
static uint8_t s_wifiHistoryDraw[WIFI_WATERFALL_ROWS][WIFI_MONITOR_BINS] = {};

static const int ROW_H = 14;
static const int TITLE_H = 16;

// Shared CYD chrome for pages that do not have a dedicated visualizer. The
// original modules use the same Nosifer glitch title, skull watermark, blue /
// pink palette and framed control area; the compact landscape layout keeps
// those elements visible above the T-Embed's 16-pixel action strip.
static const unsigned char *iconForTitle(const char *title) {
    if (strstr(title, "WiFi") || strstr(title, "WIFI") || strstr(title, "Karma") ||
        strstr(title, "EAPOL") || strstr(title, "Wardriv") ||
        strstr(title, "Flock") || strstr(title, "IoT"))
        return bitmap_icon_skull_wifi;
    if (strstr(title, "BLE") || strstr(title, "Bluetooth") ||
        strstr(title, "Airoha") || strstr(title, "Whisper") ||
        strstr(title, "Lunatic") || strstr(title, "Skeleton"))
        return bitmap_icon_skull_bluetooth;
    if (strstr(title, "Sub") || strstr(title, "Brute") ||
        strstr(title, "Replay") || strstr(title, "Tesla"))
        return bitmap_icon_skull_subghz;
    if (strstr(title, "NRF") || strstr(title, "2.4GHz") ||
        strstr(title, "WLAN") || strstr(title, "Proto") || strstr(title, "Mouse"))
        return bitmap_icon_skull_jammer;
    if (strstr(title, "RFID") || strstr(title, "Card"))
        return bitmap_icon_skull_rfid;
    if (strstr(title, "Setting") || strstr(title, "Brightness") ||
        strstr(title, "Rotation") || strstr(title, "Color") ||
        strstr(title, "PIN") || strstr(title, "Timeout"))
        return bitmap_icon_skull_setting;
    return bitmap_icon_skull_tools;
}

static void drawClippedText(const char *value, int x, int y, int maxWidth,
                            uint16_t fg, uint16_t bg) {
    if (!value || maxWidth <= 0) return;
    char buf[96];
    strlcpy(buf, value, sizeof(buf));
    while (buf[0] && sprite.textWidth(buf) > maxWidth)
        buf[strlen(buf) - 1] = '\0';
    sprite.setTextColor(fg, bg);
    sprite.setCursor(x, y);
    sprite.print(buf);
}

static void renderChrome(const char *title, const char *mode) {
    sprite.fillSprite(TFT_BLACK);
    sprite.drawBitmap((sprite.width() - SKULL_BG_WIDTH) / 2, -68,
                      skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0021);
    sprite.fillRect(0, 0, sprite.width(), 17, HH_DARK);
    sprite.drawBitmap(8, 0, bitmap_icon_go_back, 16, 16, HH_BLUE);
    sprite.setTextSize(1);
    drawClippedText(mode, 30, 4, sprite.width() - 101, HH_BLUE, HH_DARK);
    sprite.drawBitmap(sprite.width() - 84, 0, iconForTitle(title), 16, 16, HH_PINK);
    sprite.drawFastHLine(0, 17, sprite.width(), HH_VIOLET);

    sprite.setFreeFont(&Nosifer_Regular10pt7b);
    int tw = sprite.textWidth(title);
    if (tw <= sprite.width() - 16) {
        int x = (sprite.width() - tw) / 2;
        sprite.setTextColor(HH_BLUE, TFT_BLACK);
        sprite.setCursor(x - 1, 37);
        sprite.print(title);
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(x + 1, 39);
        sprite.print(title);
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.setCursor(x, 38);
        sprite.print(title);
    } else {
        sprite.setFreeFont(NULL);
        sprite.setTextSize(2);
        drawClippedText(title, 9, 23, sprite.width() - 18, TFT_WHITE, TFT_BLACK);
        sprite.setTextSize(1);
    }
    sprite.setFreeFont(NULL);
    sprite.drawFastHLine(0, 44, sprite.width(), HH_PINK);
}

static void renderActionFooter(const char *hint, int page, int pages) {
    int y = sprite.height() - 16;
    sprite.fillRect(0, y, sprite.width(), 16, HH_DARK);
    sprite.drawFastHLine(0, y, sprite.width(), HH_VIOLET);
    sprite.setTextSize(1);
    char pageText[20] = {};
    if (pages > 0) snprintf(pageText, sizeof(pageText), "%d/%d", page, pages);
    int pageWidth = (int)strlen(pageText) * 6;
    drawClippedText(hint && hint[0] ? hint :
#ifdef BOARD_M5STICK_S3
                    "Hold B Back",
#else
                    "SIDE Back",
#endif
                    5, y + 5,
                    sprite.width() - pageWidth - 17, TFT_LIGHTGREY, HH_DARK);
    if (pageWidth) drawClippedText(pageText, sprite.width() - pageWidth - 5,
                                   y + 5, pageWidth, HH_PINK, HH_DARK);
}

// Menu icons remain the exact 16x16 CYD bitmaps. Data pages use the same
// framing, with enough rows for file content and live result lists.
static void renderList(const char *title, const char *const *items,
                       const unsigned char *const *icons, int count, int selected,
                       const char *hint) {
    const bool menu = icons != NULL;
    if (count < 0) count = 0;
    if (selected < 0) selected = 0;
    if (count && selected >= count) selected = count - 1;
#ifdef BOARD_M5STICK_S3
    renderChrome(title, menu ? "MENU A/B MOVE" : "RESULTS A/B MOVE");
#else
    renderChrome(title, menu ? "MENU  ROT=SELECT" : "RESULTS  ROT=BROWSE");
#endif
    const int rowH = menu ? 20 : 14;
    const int top = 47;
    const int visible = (sprite.height() - 16 - top) / rowH;
    int first = selected >= visible ? selected - visible + 1 : 0;
    if (first > count - visible && count > visible) first = count - visible;

    if (!count || !items) {
        sprite.setTextSize(1);
        drawClippedText("No entries", 10, 91, sprite.width() - 20,
                        HH_GUNMETAL, TFT_BLACK);
    }
    for (int row = 0; row < visible && first + row < count && items; ++row) {
        int i = first + row;
        int y = top + row * rowH;
        bool active = i == selected;
        uint16_t bg = active ? HH_DARK : TFT_BLACK;
        if (active) {
            sprite.fillRect(2, y, sprite.width() - 8, rowH - 1, bg);
            sprite.fillRect(2, y, 3, rowH - 1, HH_PINK);
        }
        if (icons) {
            sprite.drawBitmap(8, y + 2, icons[i], 16, 16,
                              active ? HH_PINK : HH_BLUE);
            drawClippedText(items[i], 29, y + 6, sprite.width() - 43,
                            active ? TFT_WHITE : HH_BLUE, bg);
        } else {
            drawClippedText(items[i], 10, y + 3, sprite.width() - 24,
                            active ? TFT_WHITE : s_colText, bg);
        }
    }

    if (count > visible) {
        const int trackH = sprite.height() - top - 18;
        int thumbH = (trackH * visible) / count;
        if (thumbH < 8) thumbH = 8;
        int offset = ((trackH - thumbH) * first) / (count - visible);
        sprite.fillRect(sprite.width() - 3, top, 2, trackH, HH_GUNMETAL);
        sprite.fillRect(sprite.width() - 3, top + offset, 2, thumbH, HH_PINK);
    }
    int pages = count ? (count + visible - 1) / visible : 1;
    int page = count ? (selected / visible) + 1 : 1;
    renderActionFooter(hint && hint[0] ? hint :
#ifdef BOARD_M5STICK_S3
                       "A/B Move | Hold A Open/B Back",
#else
                       (menu ? "ROT Select | PRESS Open | SIDE Back"
                             : "ROT Browse | PRESS Select | SIDE Back"),
#endif
                       page, pages);
}

// Main menu — CYD's 2-column grid: skull icon centered above centered label,
// magenta for idle, hotpink for the selection, VALHALLA banner at the bottom.
static void renderMainMenu(const char *const *titles, int count, int selected) {
    sprite.fillSprite(TFT_BLACK);

#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    // 6 items (WiFi/BLE-only menu, menu_data.h's CARDPUTER_MAIN_MENU) fit a
    // 2x3 grid comfortably on the 135px-tall panel — the T-Embed's 2x5 grid
    // below assumes its 170px-tall landscape canvas and would run the last
    // row's icons straight into the VALHALLA banner here.
    const int COLS = 2;
    const int ROWS = 3;
    const int CELL_W = sprite.width() / COLS;
    const int Y_START = 6;
    const int Y_SPACING = 34;
    const int BANNER_H = 14;
    const unsigned char *const *menuIcons = CARDPUTER_MAIN_MENU_ICONS;
#else
    const int COLS = 2;
    const int ROWS = 5;
    const int CELL_W = sprite.width() / COLS;
    const int Y_START = 2;
    const int Y_SPACING = 29;
    const int BANNER_H = 20;
    const unsigned char *const *menuIcons = MAIN_MENU_ICONS;
#endif

    for (int i = 0; i < count && i < COLS * ROWS; i++) {
        int col = i / ROWS;
        int row = i % ROWS;
        int x = col * CELL_W;
        int y = Y_START + row * Y_SPACING;
        bool sel = (i == selected);
        uint16_t color = sel ? HH_PINK : HH_BLUE;

        sprite.drawBitmap(x + (CELL_W - 16) / 2, y + 2, menuIcons[i], 16, 16, color);

        sprite.setTextSize(1);
        sprite.setTextColor(color, TFT_BLACK);
        int tw = strlen(titles[i]) * 6;
        sprite.setCursor(x + (CELL_W - tw) / 2, y + 20);
        sprite.print(titles[i]);
    }

    // VALHALLA banner (same double border + glitch as the CYD home screen).
    int barY = sprite.height() - BANNER_H - 2;
    sprite.fillRect(2, barY, sprite.width() - 4, BANNER_H, HH_DARK);
    sprite.drawRect(2, barY, sprite.width() - 4, BANNER_H, HH_BLUE);
    sprite.drawRect(4, barY + 2, sprite.width() - 8, BANNER_H - 4, HH_VIOLET);

    sprite.setFreeFont(&Nosifer_Regular10pt7b);
    sprite.setTextSize(1);
    const char *label = "VALHALLA";
    int tw = sprite.textWidth(label);
    int tx = (sprite.width() - tw) / 2;
    int ty = barY + BANNER_H - 5;
    sprite.setTextColor(HH_BLUE, HH_DARK);
    sprite.setCursor(tx - 1, ty - 1);
    sprite.print(label);
    sprite.setTextColor(HH_PINK, HH_DARK);
    sprite.setCursor(tx + 1, ty + 1);
    sprite.print(label);
    sprite.setTextColor(TFT_WHITE, HH_DARK);
    sprite.setCursor(tx, ty);
    sprite.print(label);
    sprite.setFreeFont(NULL);

    // Lock indicator when the PIN is enabled (CYD: eye2 top-right).
    if (g_settings.pinEnabled) {
        sprite.drawBitmap(sprite.width() - 20, 16, bitmap_icon_eye2, 16, 16, HH_PINK);
    }
}

static void renderPlaceholder(const char *title) {
    renderChrome(title, "MODULE STATUS");
    sprite.drawRoundRect(9, 57, sprite.width() - 18, 79, 4, HH_VIOLET);
    sprite.drawRoundRect(10, 58, sprite.width() - 20, 77, 3, HH_GUNMETAL);
    sprite.setTextSize(1);
    drawClippedText("PORT PENDING", 19, 69, sprite.width() - 38,
                    HH_PINK, TFT_BLACK);
    drawClippedText(
#ifdef BOARD_M5STICK_S3
                    "Unavailable on StickS3",
#else
                    "This module is not available on T-Embed yet.",
#endif
                    19, 91, sprite.width() - 38, TFT_WHITE, TFT_BLACK);
    renderActionFooter(nullptr, 0, 0);
}

static const char *statusMode(const char *title, const char *message) {
    if (strstr(message, "failed") || strstr(message, "FAILED") ||
        strstr(message, "ERROR") || strstr(message, "NOT FOUND")) return "ERROR";
    if (strstr(message, "starting") || strstr(message, "Starting") ||
        strstr(message, "working") || strstr(message, "Connecting")) return "INIT";
    if (strstr(message, "scanning") || strstr(message, "Scanning") ||
        strstr(message, "SCAN")) return "SCANNING";
    if (strstr(message, "listening") || strstr(message, "Listening") ||
        strstr(message, "Monitoring") || strstr(message, "LIVE")) return "LISTENING";
    if (strstr(message, "TX") || strstr(message, "Sending") ||
        strstr(message, "JAMMING") || strstr(message, "ACTIVE")) return "ACTIVE";
    if (strstr(message, "Ready") || strstr(message, "READY") ||
        strstr(message, "Standby") || strstr(message, "IDLE")) return "READY";
    if (strstr(message, "CAP:") || strstr(message, "CAPTURED")) return "CAPTURED";
    if (strstr(title, "Guardian") || strstr(title, "Sentinel") ||
        strstr(title, "Capture")) return "LISTENING";
    if (strstr(title, "Wardriv")) return "SCANNING";
    if (strstr(title, "Brute") || strstr(title, "Jammer") ||
        strstr(title, "Spoofer") || strstr(title, "Beacon") ||
        strstr(title, "Proto Kill")) return "ACTIVE";
    if (strstr(title, "Brightness") || strstr(title, "Timeout") ||
        strstr(title, "Color") || strstr(title, "Rotation") ||
        strstr(title, "PIN") || strstr(title, "Invert")) return "SETTINGS";
    return "STATUS";
}

// These compact readouts use only values reported by the worker. The CYD
// originals show the same capture phases, threat rates and scan counters in
// their larger live panels; no decorative data is invented here.
static bool renderStatusDetails(const char *title, const char *message) {
    if (sprite.width() < 300) return false;
    if (strcmp(title, "EAPOL Capture") == 0 && strncmp(message, "P:", 2) == 0) {
        unsigned long packets = 0, eapol = 0;
        char steps[5] = {};
        char pmk = 'n', hs = 'n';
        if (sscanf(message, "P:%lu E:%lu M:%4s PMK:%c HS:%c",
                   &packets, &eapol, steps, &pmk, &hs) != 5) return false;
        char counts[44];
        snprintf(counts, sizeof(counts), "PACKETS %lu    EAPOL %lu", packets, eapol);
        drawClippedText(counts, 25, 83, sprite.width() - 50, HH_BLUE, TFT_BLACK);
        for (int i = 0; i < 4; ++i) {
            int x = 25 + i * 27;
            bool seen = steps[i] == ('1' + i);
            sprite.fillRoundRect(x, 101, 23, 19, 2, seen ? HH_DARK : TFT_BLACK);
            sprite.drawRoundRect(x, 101, 23, 19, 2, seen ? HH_PINK : HH_GUNMETAL);
            char label[4];
            snprintf(label, sizeof(label), "M%d", i + 1);
            drawClippedText(label, x + 5, 107, 13,
                            seen ? TFT_WHITE : HH_GUNMETAL,
                            seen ? HH_DARK : TFT_BLACK);
        }
        drawClippedText(pmk == 'Y' ? "PMKID YES" : "PMKID NO", 146, 103, 68,
                        pmk == 'Y' ? HH_PINK : HH_BLUE, TFT_BLACK);
        drawClippedText(hs == 'Y' ? "HS YES" : "HS NO", 224, 103, 65,
                        hs == 'Y' ? HH_PINK : HH_BLUE, TFT_BLACK);
        return true;
    }
    if (strcmp(title, "WiFi Guardian") == 0 && strncmp(message, "Ch", 2) == 0) {
        int channel = 0;
        char threat[18] = {};
        unsigned long deauth = 0, disassoc = 0, beacon = 0;
        if (sscanf(message, "Ch%d %17s D:%lu Ds:%lu B:%lu", &channel, threat,
                   &deauth, &disassoc, &beacon) != 5) return false;
        char line[56];
        snprintf(line, sizeof(line), "CH %d   THREAT %s", channel, threat);
        drawClippedText(line, 25, 84, sprite.width() - 50, HH_PINK, TFT_BLACK);
        snprintf(line, sizeof(line), "DEAUTH %lu   DISASSOC %lu   BEACON %lu",
                 deauth, disassoc, beacon);
        drawClippedText(line, 25, 106, sprite.width() - 50, TFT_WHITE, TFT_BLACK);
        return true;
    }
    if (strcmp(title, "SubGHz Sentinel") == 0 && strncmp(message, "Flagged:", 8) == 0) {
        int flagged = 0, total = 0;
        char threat[18] = {};
        if (sscanf(message, "Flagged:%d/%d %17s", &flagged, &total, threat) != 3)
            return false;
        char line[52];
        snprintf(line, sizeof(line), "FLAGGED %d / %d    %s", flagged, total, threat);
        drawClippedText(line, 25, 84, sprite.width() - 50, HH_PINK, TFT_BLACK);
        sprite.drawRoundRect(25, 105, sprite.width() - 57, 12, 2, HH_VIOLET);
        int fill = total > 0 ? (sprite.width() - 61) * flagged / total : 0;
        if (fill < 0) fill = 0;
        if (fill > sprite.width() - 61) fill = sprite.width() - 61;
        if (fill) sprite.fillRect(27, 107, fill, 8, HH_PINK);
        return true;
    }
    if (strcmp(title, "Wardriving") == 0 && strncmp(message, "Logged:", 7) == 0) {
        unsigned long logged = 0, duplicates = 0;
        if (sscanf(message, "Logged:%lu Dupes:%lu", &logged, &duplicates) != 2)
            return false;
        char line[52];
        snprintf(line, sizeof(line), "LOGGED %lu", logged);
        drawClippedText(line, 25, 85, 128, HH_PINK, TFT_BLACK);
        snprintf(line, sizeof(line), "DUPES %lu", duplicates);
        drawClippedText(line, 164, 85, sprite.width() - 186, HH_BLUE, TFT_BLACK);
        drawClippedText("SD scan log", 25, 107, sprite.width() - 50,
                        TFT_WHITE, TFT_BLACK);
        return true;
    }
    return false;
}

static void renderStatus(const char *title, const char *message, const char *hint) {
    const char *mode = statusMode(title, message);
    renderChrome(title, mode);
    const bool error = strcmp(mode, "ERROR") == 0;
    const bool idle = strcmp(mode, "READY") == 0 || strcmp(mode, "STATUS") == 0;
    uint16_t stateColor = error ? TFT_RED : idle ? HH_BLUE : HH_PINK;

    char stateLabel[28];
    snprintf(stateLabel, sizeof(stateLabel), ">> %s <<", mode);
    sprite.setTextSize(1);
    int stateWidth = sprite.textWidth(stateLabel);
    drawClippedText(stateLabel, (sprite.width() - stateWidth) / 2, 56,
                    sprite.width() - 20, stateColor, TFT_BLACK);

    const bool compact = sprite.height() < 150;
    const int boxHeight = compact ? 43 : 53;
    sprite.drawRoundRect(9, 75, sprite.width() - 18, boxHeight, 4, HH_VIOLET);
    sprite.drawRoundRect(10, 76, sprite.width() - 20, boxHeight - 2, 3, HH_GUNMETAL);
    sprite.fillRect(15, 81, 3, boxHeight - 12, stateColor);
    if (!renderStatusDetails(title, message)) {
        drawClippedText("STATUS", 25, 82, 65, HH_BLUE, TFT_BLACK);
        char line[RENDER_MSG_TEXT_MAX];
        char secondLine[RENDER_MSG_TEXT_MAX] = {};
        strlcpy(line, message, sizeof(line));
        bool hasSecond = false;
        if (sprite.textWidth(line) > sprite.width() - 55) {
            int cut = (sprite.width() - 55) / 6;
            if (cut >= (int)sizeof(line)) cut = sizeof(line) - 1;
            int word = cut;
            while (word > cut / 2 && line[word] != ' ') --word;
            if (word > cut / 2) cut = word;
            strlcpy(secondLine, line + cut + (line[cut] == ' ' ? 1 : 0),
                    sizeof(secondLine));
            line[cut] = '\0';
            hasSecond = true;
        }
        drawClippedText(line, 25, compact ? (hasSecond ? 96 : 102) : (hasSecond ? 99 : 104),
                        sprite.width() - 45,
                        error ? HH_PINK : TFT_WHITE, TFT_BLACK);
        if (hasSecond) drawClippedText(secondLine, 25, compact ? 108 : 113,
                                       sprite.width() - 45,
                                       error ? HH_PINK : TFT_WHITE, TFT_BLACK);
    }

    const char *percentMark = strchr(message, '%');
    // The 135-pixel panel ends at the action footer; these indicators fit
    // only on the taller T-Embed display.
    if (!compact && percentMark && percentMark > message) {
        const char *digits = percentMark;
        while (digits > message && digits[-1] >= '0' && digits[-1] <= '9') --digits;
        int pct = atoi(digits);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        sprite.drawRoundRect(14, 136, sprite.width() - 28, 11, 3, HH_VIOLET);
        sprite.fillRect(16, 138, (sprite.width() - 32) * pct / 100, 7,
                        stateColor);
    } else if (!compact) {
        int lit = error ? 1 : idle ? 3 : 8;
        for (int i = 0; i < 8; ++i)
            sprite.drawBitmap(14 + i * 25, 135, bitmap_icon_skull, 16, 16,
                              i < lit ? stateColor : HH_GUNMETAL);
    }
    renderActionFooter(hint, 0, 0);
}

static void renderWifiHeader(const char *title, const char *mode) {
    renderChrome(title, mode);
}

static void renderWifiFooter(const char *hint) {
    renderActionFooter(hint, 0, 0);
}

static void renderWifiStatus(const char *title, const char *message, const char *hint) {
    const char *mode = "ACTIVE";
    if (strstr(message, "Scanning") || strstr(message, "scanning")) mode = "SCAN";
    else if (strstr(message, "Listening") || strstr(message, "Sniff")) mode = "LIVE";
    else if (strstr(message, "Starting") || strstr(message, "starting")) mode = "INIT";
    else if (strstr(message, "failed") || strstr(message, "FAILED")) mode = "ERROR";
    renderWifiHeader(title, mode);
    sprite.drawBitmap((sprite.width() - SKULL_BG_WIDTH) / 2, -78,
                      skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0041);
    sprite.fillRect(0, 46, sprite.width(), 13, HH_DARK);
    sprite.setTextSize(1);
    sprite.setTextColor(HH_BLUE, HH_DARK);
    sprite.setCursor(5, 49);
    sprite.print("STATUS");

    int phase = (millis() / 180) % 8;
    for (int i = 0; i < 8; i++) {
        sprite.drawBitmap(17 + i * 23, 75, bitmap_icon_skull, 16, 16,
                          i <= phase ? HH_PINK : HH_GUNMETAL);
    }
    drawClippedText(message, 17, 105, sprite.width() - 34,
                    TFT_WHITE, TFT_BLACK);
    renderWifiFooter(hint);
}

static void renderWifiSignalBars(int x, int y, int rssi, bool selected) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;
    for (int i = 0; i < 5; i++) {
        int h = 2 + i * 2;
        sprite.fillRect(x + i * 4, y + 11 - h, 3, h,
                        i < bars ? (selected ? HH_PINK : HH_BLUE) : HH_GUNMETAL);
    }
}

static bool parseWifiApLine(const char *line, char *name, size_t nameLen,
                            int *rssi, int *channel) {
    const char *ch = strstr(line, " CH");
    const char *dbm = strstr(line, "dBm");
    if (!ch || !dbm || dbm >= ch) return false;
    const char *rssiStart = dbm;
    while (rssiStart > line && rssiStart[-1] != ' ') rssiStart--;
    size_t n = (size_t)(rssiStart - line);
    while (n > 0 && line[n - 1] == ' ') n--;
    if (n >= nameLen) n = nameLen - 1;
    memcpy(name, line, n);
    name[n] = '\0';
    *rssi = atoi(rssiStart);
    *channel = atoi(ch + 3);
    return true;
}

static void renderWifiList(const char *title, const char *const *items,
                           int count, int selected, const char *hint) {
    char mode[20];
#ifdef BOARD_M5STICK_S3
    snprintf(mode, sizeof(mode), "%d/%d A/B MOVE", selected + 1, count);
#else
    snprintf(mode, sizeof(mode), "%d/%d  ROT=SEL", selected + 1, count);
#endif
    renderWifiHeader(title, mode);

#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    // 240px wide, not 320 — the T-Embed's fixed x=290/292 CH column sits
    // off the right edge here, and its 6-row list (starting at y=59, 15px
    // each) runs into the footer on this 135px-tall panel. Column x scales
    // off sprite.width() instead of a literal T-Embed-canvas pixel value,
    // and the row count is derived from the space actually available
    // above the footer rather than a fixed constant.
    const int chColX = sprite.width() - 26;
    const int rowH = 13;
    const int listTop = 58; // just below the 45..58 column-header bar
    const int footerY = sprite.height() - 16;
    const int visible = (footerY - listTop) / rowH;
#else
    const int chColX = 290;
    const int rowH = 15;
    const int listTop = 59;
    const int visible = 6;
#endif

    sprite.fillRect(0, 45, sprite.width(), 13, HH_DARK);
    sprite.setTextColor(HH_BLUE, HH_DARK);
    sprite.setCursor(5, 48);
    sprite.print("SIG");
    sprite.setCursor(32, 48);
    sprite.print("NETWORK / EVENT");
    sprite.setCursor(chColX, 48);
    sprite.print("CH");

    int first = 0;
    if (selected >= visible) first = selected - visible + 1;
    for (int row = 0; row < visible && first + row < count; row++) {
        int i = first + row;
        int y = listTop + row * rowH;
        bool sel = i == selected;
        if (sel) {
            sprite.fillRect(0, y - 1, sprite.width(), rowH - 1, HH_DARK);
        }
        sprite.setTextSize(1);
        char network[34];
        int rssi = 0, channel = 0;
        if (parseWifiApLine(items[i], network, sizeof(network), &rssi, &channel)) {
            renderWifiSignalBars(5, y, rssi, sel);
            sprite.setTextColor(sel ? HH_PINK : TFT_WHITE, sel ? HH_DARK : TFT_BLACK);
            sprite.setCursor(32, y + 2);
            sprite.print(network);
            sprite.setTextColor(sel ? HH_PINK : HH_VIOLET, sel ? HH_DARK : TFT_BLACK);
            sprite.setCursor(chColX + 2, y + 2);
            sprite.print(channel);
        } else {
            sprite.setTextColor(sel ? HH_PINK : HH_BLUE, sel ? HH_DARK : TFT_BLACK);
            sprite.setCursor(5, y + 2);
            sprite.print(items[i]);
        }
    }

    int pages = (count + visible - 1) / visible;
    if (pages < 1) pages = 1;
    int page = selected / visible + 1;
    renderActionFooter(hint, page, pages);
}

static uint16_t wifiLevelColor(uint8_t level) {
    // Exact four-stage palette from wifi_attacks.cpp's PacketMonitor:
    // black -> deep purple -> electric blue -> hot pink -> white.
    int k = constrain(level, 0, 127);
    int r, g, b;
    if (k < 32) {
        r = (k * 15) / 31;
        g = 0;
        b = (k * 20) / 31;
    } else if (k < 64) {
        int t = k - 32;
        r = 15 - (t * 15) / 31;
        g = t;
        b = 20 + (t * 11) / 31;
    } else if (k < 96) {
        int t = k - 64;
        r = t;
        g = 31 - t;
        b = 31;
    } else {
        int t = k - 96;
        r = 31;
        g = (t * 63) / 31;
        b = 31;
    }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void renderWifiMonitor(const WifiMonitorMsg &state,
                              const uint8_t history[WIFI_WATERFALL_ROWS][WIFI_MONITOR_BINS]) {
    sprite.fillSprite(TFT_BLACK);

    // Original CYD icon bar: BACK on the left, live channel/packet status in
    // the middle, and CH+/CH- controls on the right. The encoder drives those
    // channel controls on T-Embed.
    sprite.fillRect(0, 0, sprite.width(), 17, HH_DARK);
    sprite.drawBitmap(10, 0, bitmap_icon_go_back, 16, 16, HH_BLUE);
    sprite.drawBitmap((170 * sprite.width()) / 240, 0,
                      bitmap_icon_sort_up_plus, 16, 16, HH_BLUE);
    sprite.drawBitmap((210 * sprite.width()) / 240, 0,
                      bitmap_icon_sort_down_minus, 16, 16, HH_BLUE);
    sprite.setTextSize(1);
    sprite.setTextColor(HH_BLUE, HH_DARK);
    sprite.setCursor(35, 4);
    sprite.printf("Ch:%u", state.channel);
    sprite.setCursor(80, 4);
    sprite.printf("Pkt:%lu", (unsigned long)state.totalPackets);
    sprite.drawFastHLine(0, 16, sprite.width(), HH_PINK);

    const int centerX = sprite.width() / 2;
    const int graphY = 19;
    const int graphH = 49;
    const int halfW = sprite.width() / 2;

    // Skull watermark behind the spectrum/waterfall, matching drawUI().
    sprite.drawBitmap((sprite.width() - SKULL_BG_WIDTH) / 2, -76,
                      skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0041);

    int lastRightY = graphY + graphH;
    int lastLeftY = graphY + graphH;
    for (int xoff = 0; xoff < halfW; xoff++) {
        int bin = (xoff * WIFI_MONITOR_BINS) / halfW;
        uint8_t level = state.spectrum[bin];
        int y = graphY + graphH - map(level, 0, 127, 0, graphH);
        uint16_t color = wifiLevelColor(level);
        if (xoff > 0) {
            int xr = centerX + xoff;
            int xl = centerX - xoff - 1;
            sprite.fillTriangle(xr - 1, graphY + graphH, xr, graphY + graphH,
                                xr - 1, lastRightY, color);
            sprite.fillTriangle(xr - 1, lastRightY, xr, graphY + graphH, xr, y, color);
            sprite.fillTriangle(xl + 1, graphY + graphH, xl, graphY + graphH,
                                xl + 1, lastLeftY, color);
            sprite.fillTriangle(xl + 1, lastLeftY, xl, graphY + graphH, xl, y, color);
        }
        lastRightY = y;
        lastLeftY = y;
    }
    sprite.drawFastHLine(0, graphY + graphH + 2, sprite.width(), HH_BLUE);

    const int waterfallY = graphY + graphH + 4;
    const int waterfallH = sprite.height() - waterfallY;
    for (int y = 0; y < waterfallH; y++) {
        int srcRow = WIFI_WATERFALL_ROWS - waterfallH + y;
        if (srcRow < 0) continue;
        for (int xoff = 0; xoff < halfW; xoff++) {
            int bin = (xoff * WIFI_MONITOR_BINS) / halfW;
            uint8_t level = history[srcRow][bin];
            if (level == 0) continue;
            uint16_t color = wifiLevelColor(level);
            sprite.drawPixel(centerX + xoff, waterfallY + y, color);
            sprite.drawPixel(centerX - xoff - 1, waterfallY + y, color);
        }
    }
}

// Battery indicator — drawn top-right over the generic screens (the
// Predator screen has its own header; splash/lock don't need it). Reads
// whatever battery_gauge.cpp's board-specific backend maintains (T-Embed's
// BQ27220 fuel gauge, Cardputer ADV's ADC divider, or StickS3's M5PM1 VBAT
// read) — percent<0 means that board reports no battery, so nothing draws.
static void renderBatteryIndicator() {
    int pct = batteryGaugePercent();
    if (pct < 0) return;

    int x = sprite.width() - 30;
    int y = 3;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    sprite.setTextSize(1);
    sprite.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    sprite.setCursor(x - 32, y + 1);
    sprite.print(buf);

    sprite.drawRect(x, y, 24, 10, TFT_LIGHTGREY);
    sprite.fillRect(x + 24, y + 3, 2, 4, TFT_LIGHTGREY);

    uint16_t color;
    if (batteryGaugeCharging()) color = TFT_CYAN;
    else if (pct <= 15) color = TFT_RED;
    else if (pct <= 30) color = TFT_YELLOW;
    else color = TFT_GREEN;

    int fillW = (pct * 20) / 100;
    if (fillW > 0) {
        sprite.fillRect(x + 2, y + 2, fillW, 6, color);
    }
}

// SubGHz spectrum heat palette: black -> blue -> cyan -> green -> yellow ->
// red across the 0-125 level range (see subghz_scan.cpp's rssiToLevel()).
static uint16_t spectrumHeatColor(int level) {
    if (level < 15) return sprite.color565(0, 0, 0);
    if (level < 35) return sprite.color565(0, 0, 40 + (level - 15) * 5);
    if (level < 55) return sprite.color565(0, (level - 35) * 12, 255);
    if (level < 75) return sprite.color565(0, 255, 255 - (level - 55) * 12);
    if (level < 95) return sprite.color565((level - 75) * 12, 255, 0);
    if (level < 115) return sprite.color565(255, 255 - (level - 95) * 12, 0);
    return sprite.color565(255, 0, 0);
}

static void renderSpectrum(const char *title, const uint8_t *bins, int count) {
    static uint8_t s_peaks[RENDER_MSG_SPECTRUM_BINS];
    int peak = 0;
    for (int i = 0; i < count && i < RENDER_MSG_SPECTRUM_BINS; ++i)
        if (bins[i] > peak) peak = bins[i];
    char mode[32];
    snprintf(mode, sizeof(mode), "LIVE  PEAK %ddBm", -85 + (peak * 60) / 125);
    renderChrome(title, mode);
    if (count <= 0) {
        renderActionFooter("SIDE Back | CC1101 RX", 0, 0);
        return;
    }
    if (count > RENDER_MSG_SPECTRUM_BINS) count = RENDER_MSG_SPECTRUM_BINS;

    const int plotLeft = 27;
    const int plotTop = 51;
    const int plotHeight = 81;
    const int plotWidth = sprite.width() - plotLeft - 5;
    sprite.fillRect(plotLeft, plotTop, plotWidth, plotHeight + 1, TFT_BLACK);
    for (int level = 0; level <= 125; level += 25) {
        int y = plotTop + plotHeight - level * plotHeight / 125;
        for (int x = plotLeft; x < plotLeft + plotWidth; x += 5)
            sprite.drawPixel(x, y, HH_GUNMETAL);
    }

    for (int i = 0; i < count; i++) {
        int level = bins[i];
        if (level > 125) level = 125;

        // Peak hold with slow decay.
        if (level >= s_peaks[i]) s_peaks[i] = level;
        else if (s_peaks[i] > 3) s_peaks[i] -= 3;
        else s_peaks[i] = 0;

        int barHeight = map(level, 0, 125, 0, plotHeight);
        int x = plotLeft + (i * plotWidth) / count;
        int nextX = plotLeft + ((i + 1) * plotWidth) / count;
        int barWidth = nextX - x - 1;
        if (barWidth < 1) barWidth = 1;
        int y = plotTop + (plotHeight - barHeight);
        sprite.fillRect(x, y, barWidth, barHeight, spectrumHeatColor(level));

        int peakY = plotTop + plotHeight - map(s_peaks[i], 0, 125, 0, plotHeight);
        sprite.drawFastHLine(x, peakY, barWidth, TFT_WHITE);
    }

    // The CC1101 sweep has three calibrated bands: 12 bins at 300-348,
    // 22 at 378-462, and 30 at 779-927 MHz. Mark their boundaries rather
    // than suggesting the large frequency gaps are continuously sampled.
    if (count == 64) {
        int boundary1 = plotLeft + 12 * plotWidth / count;
        int boundary2 = plotLeft + 34 * plotWidth / count;
        for (int y = plotTop; y < plotTop + plotHeight; y += 4) {
            sprite.drawPixel(boundary1, y, HH_VIOLET);
            sprite.drawPixel(boundary2, y, HH_VIOLET);
        }
    }
    sprite.setTextSize(1);
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(0, plotTop - 2);
    sprite.print("-25");
    sprite.setCursor(0, plotTop + plotHeight - 8);
    sprite.print("-85");
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(plotLeft, 138); sprite.print("315");
    sprite.setCursor(plotLeft + plotWidth * 26 / 64 - 9, 138); sprite.print("433");
    sprite.setCursor(plotLeft + plotWidth * 51 / 64 - 9, 138); sprite.print("868");
    sprite.setCursor(sprite.width() - 22, 138); sprite.print("915");
    renderActionFooter("SIDE Back | CC1101 RX", 0, 0);
}

// ─────────────────────────────────────────────────────────────────────────
// 2.4GHz NRF24 scanner — port of nrf24_attacks.cpp's Scanner UI
// (drawScannerFrame()/drawBarGraph()): 85-channel bar graph over
// 2400-2484MHz, dashed WiFi 1/6/11/13 markers (NRF channels 12/37/62/72),
// teal->hotpink gradient bars, PEAK readout and the animated 8-skull signal
// meter. The original's Calibrate/Refresh touch icons map to the encoder's
// long-press/short-press (see nrf_scanner.h).
// ─────────────────────────────────────────────────────────────────────────
static uint16_t nrfBarColor(int height, int maxHeight) {
    float ratio = (float)height / (float)maxHeight;
    if (ratio > 1.0f) ratio = 1.0f;

    // Teal RGB(0, 207, 255) -> Hot Pink RGB(255, 28, 82), same as the
    // original's getBarColor().
    uint8_t r = (uint8_t)(ratio * 255);
    uint8_t g = 207 - (uint8_t)(ratio * (207 - 28));
    uint8_t b = 255 - (uint8_t)(ratio * (255 - 82));
    return sprite.color565(r, g, b);
}

static const unsigned char *const NRF_SCANNER_SKULLS[8] = {
    bitmap_icon_skull_wifi, bitmap_icon_skull_bluetooth,
    bitmap_icon_skull_jammer, bitmap_icon_skull_subghz,
    bitmap_icon_skull_ir, bitmap_icon_skull_tools,
    bitmap_icon_skull_setting, bitmap_icon_skull_about
};

static void renderNrfScanner(const uint8_t *bins, int count) {
    static int s_skullFrame = 0;

    sprite.fillSprite(TFT_BLACK);

    sprite.setTextSize(1);
    sprite.setTextColor(s_colTitle, TFT_BLACK);
    sprite.setCursor(2, 2);
    sprite.print("2.4GHz Scanner");
    sprite.drawFastHLine(0, TITLE_H, sprite.width(), TFT_DARKGREY);

    if (count <= 0) return;
    if (count > RENDER_MSG_SPECTRUM_BINS) count = RENDER_MSG_SPECTRUM_BINS;

    const int plotLeft = 16;
    const int plotTop = 26;
    const int plotHeight = 88;
    const int axisY = plotTop + plotHeight;
    const int plotWidth = sprite.width() - plotLeft - 2;
    int barStride = plotWidth / count;
    if (barStride < 1) barStride = 1;
    int barWidth = (barStride > 1) ? barStride - 1 : 1;

    // Axes.
    sprite.drawFastVLine(plotLeft - 2, plotTop, plotHeight, s_colHighlight);
    sprite.drawFastHLine(plotLeft, axisY, plotWidth, s_colHighlight);

    // WiFi channel markers — NRF ch 12/37/62/72 = WiFi 1/6/11/13.
    static const int WIFI_MARKERS[4] = {12, 37, 62, 72};
    int markerX[4];
    for (int m = 0; m < 4; m++) {
        markerX[m] = plotLeft + WIFI_MARKERS[m] * plotWidth / count;
        uint16_t c = (m == 3) ? HH_VIOLET : HH_PINK;
        for (int y = plotTop; y < axisY; y += 6) {
            sprite.drawPixel(markerX[m], y, c);
        }
    }
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(markerX[0] - 2, plotTop - 9);
    sprite.print("1");
    sprite.setCursor(markerX[1] - 2, plotTop - 9);
    sprite.print("6");
    sprite.setCursor(markerX[2] - 6, plotTop - 9);
    sprite.print("11");
    sprite.setTextColor(HH_VIOLET, TFT_BLACK);
    sprite.setCursor(markerX[3] - 6, plotTop - 9);
    sprite.print("13");

    // Bars (drawn tallest-last so the peak tracker sees every channel).
    int peakChannel = 0;
    uint8_t peakLevel = 0;
    for (int ch = 0; ch < count; ch++) {
        uint8_t level = bins[ch];
        if (level > peakLevel) {
            peakLevel = level;
            peakChannel = ch;
        }
        if (level == 0) continue;

        int x = plotLeft + ch * plotWidth / count;
        int barH = (level * plotHeight) / 125;
        if (barH > plotHeight) barH = plotHeight;
        if (barH < 4) barH = 4;
        int barY = axisY - barH;

        for (int y = 0; y < barH; y++) {
            sprite.drawFastHLine(x, barY + barH - 1 - y, barWidth,
                                 nrfBarColor(y, plotHeight));
        }
    }

    // Frequency labels under the axis.
    sprite.setTextColor(s_colHighlight, TFT_BLACK);
    sprite.setCursor(plotLeft - 5, axisY + 2);
    sprite.print("2400");
    sprite.setCursor(plotLeft + plotWidth / 2 - 12, axisY + 2);
    sprite.print("2442");
    sprite.setCursor(plotLeft + plotWidth - 28, axisY + 2);
    sprite.print("2484");

    // PEAK readout.
    int statusY = axisY + 16;
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(4, statusY);
    sprite.print("PEAK: ");
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.printf("%d MHz", 2400 + peakChannel);

    // 8-skull signal meter — original lights (peakLevel * 8) / 4, which
    // saturates at any level >= 4; the correct 0-125 -> 0-8 map is used here.
    int litSkulls = (peakLevel * 8) / 125;
    for (int i = 0; i < 8; i++) {
        int x = 4 + i * 18;
        int y = statusY + 12;
        if (i < litSkulls && peakLevel > 0) {
            int phase = (s_skullFrame + i) % 8;
            float ratio = (phase < 4) ? (phase / 3.0f) : ((phase - 4) / 3.0f);
            uint8_t r, g, b;
            if (phase < 4) {
                r = (uint8_t)(ratio * 255);
                g = 207 - (uint8_t)(ratio * (207 - 28));
                b = 255 - (uint8_t)(ratio * (255 - 82));
            } else {
                r = 255 - (uint8_t)(ratio * 255);
                g = 28 + (uint8_t)(ratio * (207 - 28));
                b = 82 + (uint8_t)(ratio * (255 - 82));
            }
            sprite.drawBitmap(x, y, NRF_SCANNER_SKULLS[i], 16, 16,
                              sprite.color565(r, g, b));
        } else {
            sprite.drawBitmap(x, y, NRF_SCANNER_SKULLS[i], 16, 16, HH_GUNMETAL);
        }
    }
    s_skullFrame++;

    int pct = (peakLevel * 100) / 125;
    if (pct > 100) pct = 100;
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(4 + 8 * 18 + 4, statusY + 16);
    sprite.printf("%d%%", pct);
}

// ─────────────────────────────────────────────────────────────────────────
// 2.4GHz NRF24 spectrum analyzer — port of nrf24_attacks.cpp's Analyzer UI:
// spectrum bars (full band or AP-locked range), a 3-row skull waterfall
// (scaled down from the original's 7 rows for the 170px-tall landscape
// panel), WiFi/AP-lock markers and the peak/RUN-HOLD footer.
// ─────────────────────────────────────────────────────────────────────────
static void renderNrfAnalyzer(const uint8_t *bins, int count, int lockedWifiCh,
                              const char *ssid, bool running) {
    static uint8_t s_waterfall[3][16];
    static uint32_t s_lastWaterfallShift = 0;
    static int s_animFrame = 0;

    const int SKULL_COLS = 16;

    sprite.fillSprite(TFT_BLACK);

    // Title — locked channel + SSID, or generic (original drawAnalyzerIconBar).
    sprite.setTextSize(1);
    sprite.setTextColor(s_colTitle, TFT_BLACK);
    sprite.setCursor(2, 2);
    if (lockedWifiCh > 0) {
        char title[28];
        snprintf(title, sizeof(title), "Ch%d: %.12s", lockedWifiCh, ssid ? ssid : "");
        sprite.print(title);
    } else {
        sprite.print("2.4GHz Analyzer");
    }
    sprite.drawFastHLine(0, TITLE_H, sprite.width(), TFT_DARKGREY);

    if (count <= 0) return;
    if (count > RENDER_MSG_SPECTRUM_BINS) count = RENDER_MSG_SPECTRUM_BINS;

    // Scan range — full band or the locked channel's 22MHz band.
    int rangeStart = 0;
    int rangeEnd = count;
    if (lockedWifiCh > 0) {
        int center = 7 + lockedWifiCh * 5;
        rangeStart = (center - 11 >= 0) ? (center - 11) : 0;
        rangeEnd = (center + 11 <= count) ? (center + 11) : count;
    }
    if (rangeEnd <= rangeStart) {
        rangeStart = 0;
        rangeEnd = count;
    }
    int rangeCount = rangeEnd - rangeStart;

    const int plotLeft = 16;
    const int plotTop = 20;
    const int plotHeight = 56;
    const int axisY = plotTop + plotHeight;
    const int plotWidth = sprite.width() - plotLeft - 2;

    sprite.drawFastVLine(plotLeft - 2, plotTop, plotHeight, s_colHighlight);
    sprite.drawFastHLine(plotLeft, axisY, plotWidth, s_colHighlight);

    // Markers — single center line when AP-locked, else the 4 WiFi markers.
    if (lockedWifiCh > 0) {
        int centerNrf = 7 + lockedWifiCh * 5;
        int x = plotLeft + (centerNrf - rangeStart) * plotWidth / rangeCount;
        for (int y = plotTop; y < axisY; y += 3) {
            sprite.drawPixel(x, y, HH_PINK);
        }
    } else {
        static const int WIFI_MARKERS[4] = {12, 37, 62, 72};
        int markerX[4];
        for (int m = 0; m < 4; m++) {
            markerX[m] = plotLeft + WIFI_MARKERS[m] * plotWidth / count;
            uint16_t c = (m == 3) ? HH_VIOLET : HH_PINK;
            for (int y = plotTop; y < axisY; y += 4) {
                sprite.drawPixel(markerX[m], y, c);
            }
        }
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(markerX[0] - 4, plotTop - 9);
        sprite.print("1");
        sprite.setCursor(markerX[1] - 4, plotTop - 9);
        sprite.print("6");
        sprite.setCursor(markerX[2] - 8, plotTop - 9);
        sprite.print("11");
        sprite.setTextColor(HH_VIOLET, TFT_BLACK);
        sprite.setCursor(markerX[3] - 8, plotTop - 9);
        sprite.print("13");
    }

    // Spectrum bars (2px wide, same as the original).
    for (int i = rangeStart; i < rangeEnd; i++) {
        int barIdx = i - rangeStart;
        int x = plotLeft + barIdx * plotWidth / rangeCount;
        int level = bins[i];
        int barH = (level * plotHeight) / 125;
        if (barH > plotHeight) barH = plotHeight;
        if (barH < 4 && level > 0) barH = 4;
        if (barH <= 0) continue;
        int barY = axisY - barH;
        for (int y = 0; y < barH; y++) {
            sprite.drawFastHLine(x, barY + barH - 1 - y, 2, nrfBarColor(y, plotHeight));
        }
    }

    // Frequency labels.
    sprite.setTextColor(s_colHighlight, TFT_BLACK);
    if (lockedWifiCh > 0) {
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "%d", 2400 + rangeStart);
        sprite.setCursor(plotLeft - 5, axisY + 2);
        sprite.print(lbl);
        snprintf(lbl, sizeof(lbl), "%d", 2407 + lockedWifiCh * 5);
        sprite.setCursor(plotLeft + plotWidth / 2 - 15, axisY + 2);
        sprite.print(lbl);
        snprintf(lbl, sizeof(lbl), "%d", 2400 + rangeEnd);
        sprite.setCursor(plotLeft + plotWidth - 25, axisY + 2);
        sprite.print(lbl);
    } else {
        sprite.setCursor(plotLeft - 5, axisY + 2);
        sprite.print("2400");
        sprite.setCursor(plotLeft + plotWidth / 2 - 15, axisY + 2);
        sprite.print("2442");
        sprite.setCursor(plotLeft + plotWidth - 25, axisY + 2);
        sprite.print("2484");
    }

    // Waterfall separator.
    sprite.drawFastHLine(0, 90, sprite.width(), HH_PINK);

    // Shift the waterfall every 100ms, matching the original's lastSkullTime
    // throttle (the render task itself runs at telemetry-frame rate).
    uint32_t now = millis();
    if (now - s_lastWaterfallShift >= 100) {
        s_lastWaterfallShift = now;

        for (int row = 2; row > 0; row--) {
            for (int col = 0; col < SKULL_COLS; col++) {
                s_waterfall[row][col] = s_waterfall[row - 1][col];
            }
        }

        int channelsPerSkull = rangeCount / SKULL_COLS;
        if (channelsPerSkull < 1) channelsPerSkull = 1;
        for (int col = 0; col < SKULL_COLS; col++) {
            int startCh = rangeStart + col * channelsPerSkull;
            int endCh = startCh + channelsPerSkull;
            if (col == SKULL_COLS - 1) endCh = rangeEnd;
            uint8_t maxLevel = 0;
            for (int ch = startCh; ch < endCh && ch < count; ch++) {
                if (bins[ch] > maxLevel) maxLevel = bins[ch];
            }
            s_waterfall[0][col] = maxLevel;
        }
        s_animFrame++;
    }

    // Draw the skull waterfall — original drawSkullWaterfall()'s row fade +
    // wave shimmer, cycling the 8 skull types across the columns.
    for (int row = 0; row < 3; row++) {
        int y = 92 + row * 18;
        for (int col = 0; col < SKULL_COLS; col++) {
            int x = 6 + col * 19;
            uint8_t level = s_waterfall[row][col];

            uint16_t color;
            if (level == 0) {
                color = HH_GUNMETAL;
            } else {
                float rowFade = 1.0f - (row * 0.12f);
                int phase = (s_animFrame + col + row) % 8;
                float waveBoost = (phase < 4) ? (phase / 4.0f) : ((8 - phase) / 4.0f);
                float signalRatio = (float)(level * 2) / 125.0f;
                if (signalRatio > 1.0f) signalRatio = 1.0f;
                float finalRatio = (signalRatio * 0.7f) + (waveBoost * 0.3f);
                finalRatio *= rowFade;
                if (finalRatio > 1.0f) finalRatio = 1.0f;

                uint8_t r = (uint8_t)(finalRatio * 255);
                uint8_t g = 207 - (uint8_t)(finalRatio * (207 - 28));
                uint8_t b = 255 - (uint8_t)(finalRatio * (255 - 82));
                color = sprite.color565(r, g, b);
            }

            sprite.drawBitmap(x, y, NRF_SCANNER_SKULLS[col % 8], 16, 16, color);
        }
    }

    // Status footer — peak readout + RUN/HOLD (original drawStatusArea()).
    int peakCh = rangeStart;
    uint8_t peakVal = 0;
    for (int i = rangeStart; i < rangeEnd && i < count; i++) {
        if (bins[i] > peakVal) {
            peakVal = bins[i];
            peakCh = i;
        }
    }
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(4, 150);
    if (lockedWifiCh > 0) {
        sprite.printf("Ch%d:%.8s Peak:%dMHz Lv:%d", lockedWifiCh,
                      ssid ? ssid : "", 2400 + peakCh, peakVal);
    } else {
        sprite.printf("Peak:%dMHz Lv:%d", 2400 + peakCh, peakVal);
    }
    sprite.setTextColor(running ? TFT_GREEN : TFT_RED, TFT_BLACK);
    sprite.setCursor(sprite.width() - 30, 150);
    sprite.print(running ? "RUN" : "HOLD");
}

// ─────────────────────────────────────────────────────────────────────────
// NRF24 promiscuous sniffer — port of nrf24_attacks.cpp's drawSnHeader()/
// drawPacketList(): header stats, ADDR/CH/TYPE/# table with hit counts and
// RPD dots, and the animated 8-skull RX meter.
// ─────────────────────────────────────────────────────────────────────────
static void renderNrfSniffer(const NrfSniffDisplayRow *rows,
                             const NrfSniffDisplayStats *stats, int selected) {
    sprite.fillSprite(TFT_BLACK);

    sprite.setTextSize(1);
    sprite.setTextColor(s_colTitle, TFT_BLACK);
    sprite.setCursor(2, 2);
    sprite.print("NRF Sniffer");
    sprite.setTextColor(stats->scanning ? HH_PINK : HH_GUNMETAL, TFT_BLACK);
    sprite.setCursor(80, 2);
    sprite.print(stats->scanning ? "SCANNING" : "STANDBY");
    sprite.drawFastHLine(0, TITLE_H, sprite.width(), TFT_DARKGREY);

    // Header stats — "CH:ALL/CH:nnn  DEV:n  FRAMES:n" (original drawSnHeader).
    char hdr[40];
    if (stats->currentCh < 0) {
        snprintf(hdr, sizeof(hdr), "CH:ALL  DEV:%d  FRAMES:%lu",
                 stats->count, (unsigned long)stats->totalFrames);
    } else {
        snprintf(hdr, sizeof(hdr), "CH:%-3d  DEV:%d  FRAMES:%lu",
                 stats->currentCh, stats->count,
                 (unsigned long)stats->totalFrames);
    }
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(4, 18);
    sprite.print(hdr);
    sprite.drawFastHLine(0, 28, sprite.width(), HH_VIOLET);

    // Table header.
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(4, 32);
    sprite.print("ADDR");
    sprite.setCursor(160, 32);
    sprite.print("CH");
    sprite.setCursor(190, 32);
    sprite.print("TYPE");
    sprite.setCursor(235, 32);
    sprite.print("#");
    sprite.drawFastHLine(0, 41, sprite.width(), HH_PINK);

    const int rowH = 18;
    const int listY = 44;
    const int visible = 5;
    int first = 0;
    if (selected >= visible) first = selected - visible + 1;

    for (int r = 0; r < visible; r++) {
        int i = first + r;
        if (i >= stats->count) break;
        int y = listY + r * rowH;
        bool sel = (i == selected);
        uint16_t bg = sel ? HH_DARK : TFT_BLACK;
        if (sel) sprite.fillRect(0, y, sprite.width(), rowH - 1, HH_DARK);

        sprite.setTextColor(sel ? HH_PINK : TFT_WHITE, bg);
        sprite.setCursor(4, y + 5);
        sprite.print(rows[i].addr);

        sprite.setTextColor(HH_BLUE, bg);
        sprite.setCursor(160, y + 5);
        sprite.printf("%3d", rows[i].channel);

        sprite.setTextColor(HH_PINK, bg);
        sprite.setCursor(190, y + 5);
        sprite.print(rows[i].type);

        sprite.setTextColor(HH_VIOLET, bg);
        sprite.setCursor(235, y + 5);
        sprite.printf("%u", (unsigned)rows[i].hits);

        if (rows[i].rpd) {
            sprite.fillCircle(300, y + 8, 2, HH_PINK);
        }
    }

    if (stats->count == 0) {
        sprite.setTextColor(HH_GUNMETAL, TFT_BLACK);
        const char *msg = "Listening for packets...";
        sprite.setCursor((sprite.width() - (int)strlen(msg) * 6) / 2, listY + 26);
        sprite.print(msg);
    }

    // Skull RX meter (original drawSnSkulls()).
    for (int i = 0; i < 8; i++) {
        int x = 4 + i * 18;
        int y = 146;
        uint16_t color;
        if (stats->scanning) {
            int phase = (s_nrfSniffFrame + i) % 8;
            float ratio = (phase < 4) ? (phase / 3.0f) : ((phase - 4) / 3.0f);
            uint8_t r, g, b;
            if (phase < 4) {
                r = (uint8_t)(ratio * 255);
                g = 207 - (uint8_t)(ratio * (207 - 28));
                b = 255 - (uint8_t)(ratio * (255 - 82));
            } else {
                r = 255 - (uint8_t)(ratio * 255);
                g = 28 + (uint8_t)(ratio * (207 - 28));
                b = 82 + (uint8_t)(ratio * (255 - 82));
            }
            color = sprite.color565(r, g, b);
        } else {
            color = HH_GUNMETAL;
        }
        sprite.drawBitmap(x, y, NRF_SCANNER_SKULLS[i], 16, 16, color);
    }
    s_nrfSniffFrame++;

    sprite.setTextColor(stats->scanning ? HH_PINK : HH_GUNMETAL, TFT_BLACK);
    sprite.setCursor(4 + 8 * 18 + 6, 150);
    sprite.print(stats->scanning ? "RX!" : "OFF");

    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    const char *hint = stats->scanning ? "PRESS=Stop  HOLD=Lock"
                                       : "PRESS=Use  HOLD=Lock";
    sprite.setCursor(sprite.width() - (int)strlen(hint) * 6 - 4, 150);
    sprite.print(hint);
}

// ─────────────────────────────────────────────────────────────────────────
// NRF24 MouseJack — port of nrf24_attacks.cpp's MouseJack screen layout
// (drawMjHeader/drawMjTargetFrame/drawMjPayloadLine/drawMjStatus), re-flowed
// for the 320x170 landscape panel. The original's touch icon bar becomes the
// selectable encoder action bar at the bottom (see mousejack.h).
// ─────────────────────────────────────────────────────────────────────────
static void renderMouseJack(const MouseJackDisplay *st) {
    sprite.fillSprite(TFT_BLACK);

    // Header — title + state (original drawMjHeader).
    sprite.setTextSize(1);
    sprite.setTextColor(s_colTitle, TFT_BLACK);
    sprite.setCursor(2, 2);
    sprite.print("MOUSEJACK");

    const char *state = "STANDBY";
    uint16_t stateCol = HH_GUNMETAL;
    if (st->scanning) { state = "SCANNING"; stateCol = HH_BLUE; }
    else if (st->injecting) { state = "INJECTING"; stateCol = HH_PINK; }
    else if (st->keystrokesSent > 0) { state = "COMPLETE"; stateCol = HH_PINK; }
    sprite.setTextColor(stateCol, TFT_BLACK);
    sprite.setCursor(80, 2); // clear of the battery indicator top-right
    sprite.print(state);
    sprite.drawFastHLine(0, TITLE_H, sprite.width(), TFT_DARKGREY);

    // Target frame (original drawMjTargetFrame).
    const int frameY = 20;
    const int frameH = 42;
    sprite.drawRoundRect(4, frameY, sprite.width() - 8, frameH, 4, HH_VIOLET);
    sprite.drawRoundRect(5, frameY + 1, sprite.width() - 10, frameH - 2, 3, HH_GUNMETAL);

    if (st->hasTarget) {
        const char *devStr;
        switch (st->deviceType) {
            case 0xC1: devStr = "KEYBOARD"; break;
            case 0xC2: devStr = "MOUSE"; break;
            case 0x0F: devStr = "HID DEVICE"; break;
            default:   devStr = "UNKNOWN"; break;
        }
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(10, frameY + 4);
        sprite.print("TARGET: ");
        sprite.setTextColor(TFT_WHITE, TFT_BLACK);
        sprite.print(devStr);

        sprite.setTextColor(HH_BLUE, TFT_BLACK);
        sprite.setCursor(10, frameY + 16);
        sprite.printf("%02X:%02X:%02X:%02X:%02X",
                      st->addr[0], st->addr[1], st->addr[2], st->addr[3], st->addr[4]);
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(190, frameY + 16);
        sprite.printf("CH:%d", st->channel);

        if (st->testValid) {
            sprite.setTextColor(st->testOk ? HH_PINK : HH_GUNMETAL, TFT_BLACK);
            sprite.setCursor(10, frameY + 28);
            sprite.printf("TEST: %s", st->testOk ? "OK" : "FAIL");
        }
    } else {
        sprite.setTextSize(2);
        sprite.setTextColor(HH_GUNMETAL, TFT_BLACK);
        const char *noTgt = "NO TARGET";
        sprite.setCursor((sprite.width() - (int)strlen(noTgt) * 12) / 2, frameY + 4);
        sprite.print(noTgt);
        sprite.setTextSize(1);
        sprite.setTextColor(HH_VIOLET, TFT_BLACK);
        const char *hint = "SCAN or sniffer handoff";
        sprite.setCursor((sprite.width() - (int)strlen(hint) * 6) / 2, frameY + 26);
        sprite.print(hint);
    }

    // Payload line (original drawMjPayloadLine).
    sprite.setTextSize(1);
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(4, 68);
    sprite.print("PAYLOAD: ");
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.print(st->payloadName);

    // Status area (original drawMjStatus): progress bar + counters.
    const int barY = 84;
    const int barH = 12;
    const int barX = 4;
    const int barW = sprite.width() - 8;

    if (st->scanning) {
        sprite.drawRoundRect(barX, barY, barW, barH, 2, HH_BLUE);
        int progress = (st->scanProgress * (barW - 4)) / 100;
        if (progress > 0) sprite.fillRoundRect(barX + 2, barY + 2, progress, barH - 4, 2, HH_BLUE);
        sprite.setTextColor(HH_BLUE, TFT_BLACK);
        sprite.setCursor(4, barY + barH + 4);
        sprite.printf("PASS %d/3  CH:%d  [%d%%]", st->scanPass, st->scanCh, st->scanProgress);
    } else if (st->injecting) {
        sprite.drawRoundRect(barX, barY, barW, barH, 2, HH_PINK);
        int progress = 0;
        if (st->keystrokesTotal > 0) {
            progress = (st->keystrokesSent * (barW - 4)) / st->keystrokesTotal;
        }
        if (progress > 0) sprite.fillRoundRect(barX + 2, barY + 2, progress, barH - 4, 2, HH_PINK);
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(4, barY + barH + 4);
        sprite.printf("TX:%d/%d  OK:%d  FAIL:%d",
                      st->keystrokesSent, st->keystrokesTotal,
                      st->packetsOk, st->packetsFail);
    } else if (st->keystrokesSent > 0) {
        sprite.setTextColor(HH_PINK, TFT_BLACK);
        sprite.setCursor(4, barY);
        sprite.print("INJECTION COMPLETE");
        sprite.setTextColor(HH_BLUE, TFT_BLACK);
        sprite.setCursor(4, barY + 14);
        sprite.printf("TX:%d  OK:%d  FAIL:%d",
                      st->keystrokesSent, st->packetsOk, st->packetsFail);
    } else {
        sprite.setTextColor(st->hasTarget ? HH_PINK : HH_GUNMETAL, TFT_BLACK);
        sprite.setCursor(4, barY);
        sprite.print(st->hasTarget ? "READY" : "SCAN FOR TARGET");
    }

    // Action bar — the original's 4 touch icons, encoder-selectable.
    const int actY = 130;
    const int actH = 20;
    const char *labels[4] = {"SCAN", "TEST", "INJECT", "PAYLOAD"};
    for (int i = 0; i < 4; i++) {
        int x = 4 + i * 79;
        const int w = 75;
        bool sel = (i == st->selectedAction);
        sprite.fillRoundRect(x, actY, w, actH, 3, sel ? HH_DARK : TFT_BLACK);
        sprite.drawRoundRect(x, actY, w, actH, 3, sel ? HH_PINK : HH_GUNMETAL);
        sprite.setTextColor(sel ? HH_PINK : HH_GUNMETAL, sel ? HH_DARK : TFT_BLACK);
        int tw = strlen(labels[i]) * 6;
        sprite.setCursor(x + (w - tw) / 2, actY + 6);
        sprite.print(labels[i]);
    }

    // Footer hint.
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.setCursor(4, actY + actH + 4);
    sprite.print("ROT=Select  PRESS=Go  HOLD=Test  SIDE=Back");
}

// ─────────────────────────────────────────────────────────────────────────
// Boot splash / info page / PIN lock
// ─────────────────────────────────────────────────────────────────────────
static void renderSplash() {
    sprite.fillSprite(TFT_BLACK);
    sprite.drawBitmap((sprite.width() - SKULL_BG_WIDTH) / 2, -54,
                      skull_bg_bitmap, SKULL_BG_WIDTH, SKULL_BG_HEIGHT, 0x0021);
    sprite.drawRoundRect(8, 28, sprite.width() - 16, 113, 4, HH_VIOLET);
    sprite.drawRoundRect(10, 30, sprite.width() - 20, 109, 3, HH_GUNMETAL);
    sprite.drawBitmap(22, 41, bitmap_icon_skull_wifi, 16, 16, HH_BLUE);
    sprite.drawBitmap(sprite.width() - 38, 41, bitmap_icon_skull_subghz,
                      16, 16, HH_PINK);

    sprite.setFreeFont(&Nosifer_Regular18pt7b);
    sprite.setTextSize(1);
    int tw = sprite.textWidth("HALEHOUND");
    int tx = (sprite.width() - tw) / 2;
    if (tx < 0) tx = 0;
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(tx - 1, 66);
    sprite.print("HALEHOUND");
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(tx + 1, 68);
    sprite.print("HALEHOUND");
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(tx, 67);
    sprite.print("HALEHOUND");
    sprite.setFreeFont(NULL);

    sprite.setTextSize(1);
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor((sprite.width() - (int)strlen(FW_DEVICE) * 6) / 2, 96);
    sprite.print(FW_DEVICE);
    sprite.setTextColor(TFT_DARKGREY, TFT_BLACK);
    sprite.setCursor((sprite.width() - (int)strlen(FW_EDITION) * 6) / 2, 110);
    sprite.print(FW_EDITION);
    sprite.drawFastHLine(19, 120, sprite.width() - 38, HH_PINK);
}

static void renderInfo(const char *title, const char *const *lines, int count) {
    renderChrome(title, "SYSTEM INFO");
    sprite.setTextSize(1);
    const bool compact = sprite.height() < 150;
    const int rowHeight = compact ? 9 : 13;
    for (int i = 0; i < count && i < 8 && lines; i++) {
        int y = 48 + i * rowHeight;
        if (i & 1) sprite.fillRect(5, y - 1, sprite.width() - 10, rowHeight, HH_DARK);
        drawClippedText(lines[i], 10, y + (compact ? 0 : 2), sprite.width() - 20,
                        i == 0 ? HH_PINK : s_colText,
                        (i & 1) ? HH_DARK : TFT_BLACK);
    }
    renderActionFooter(nullptr, 0, 0);
}

static void renderLock(const char *mask) {
    renderChrome("HALEHOUND", "SECURE ACCESS");
    sprite.drawRoundRect(10, 55, sprite.width() - 20, 85, 4, HH_VIOLET);
    sprite.drawRoundRect(11, 56, sprite.width() - 22, 83, 3, HH_GUNMETAL);
    sprite.setTextSize(1);
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(24, 65);
    sprite.print("Enter PIN");

    sprite.setTextSize(3);
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(24, 87);
    sprite.print(mask);

    sprite.setTextSize(1);
#ifdef BOARD_M5STICK_S3
    renderActionFooter("A/B Digit | Hold A Next", 0, 0);
#else
    renderActionFooter("ROT Digit | PRESS Next", 0, 0);
#endif
}

// ─────────────────────────────────────────────────────────────────────────
// BLE Predator list — screen-for-screen port of the CYD build's
// drawHeader()/drawColumnHeaders()/drawDeviceList() layout: glitch title,
// RSSI bars / DEVICE / WHAT columns, tier-aware colors, RED "!" marker,
// active dot, stats line and page-indicator footer.
// ─────────────────────────────────────────────────────────────────────────
static void renderBleBars(int x, int y, int rssi) {
    int bars = 0;
    if (rssi > -40) bars = 5;
    else if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else if (rssi > -90) bars = 1;

    for (int i = 0; i < 5; i++) {
        int barH = 3 + i * 2;
        int barY = y + (12 - barH);
        sprite.fillRect(x + i * 4, barY, 3, barH, (i < bars) ? HH_PINK : HH_GUNMETAL);
    }
}

static void renderBleDevices(const BleDisplayRow *rows, const BleDisplayStats *stats, int selected) {
    const int BLE_COL_HDR_Y = 20, BLE_COL_HDR_H = 12;
    const int BLE_ROW_H = 16;
    const int BLE_LIST_Y = BLE_COL_HDR_Y + BLE_COL_HDR_H;
    const int BLE_STATS_H = 12, BLE_FOOTER_H = 12;
    const int BLE_LIST_H = sprite.height() - BLE_LIST_Y - BLE_STATS_H - BLE_FOOTER_H;
    const int visible = BLE_LIST_H / BLE_ROW_H;

    // Column x positions — the T-Embed's literal 320-canvas pixel values
    // (160/220/272) put the elapsed timer and "!" marker off the right
    // edge entirely on this 240px-wide panel (272 > 240), so Cardputer
    // gets its own explicit positions rather than a shared formula that
    // could drift T-Embed's own (hardware-verified) layout.
#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    const int colWhatX  = 132;
    const int colBangX  = sprite.width() - 8;
    const int colTimeX  = sprite.width() - 34;
#else
    const int colWhatX  = 160;
    const int colBangX  = 272;
    const int colTimeX  = 220;
#endif

    sprite.fillSprite(TFT_BLACK);

    // Title — glitch "PREDATOR", same 3-pass chromatic aberration as the
    // original's drawGlitchText().
    sprite.setFreeFont(&Nosifer_Regular10pt7b);
    sprite.setTextSize(1);
    int tw = sprite.textWidth("PREDATOR");
    int tx = (sprite.width() - tw) / 2;
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(tx - 1, 15);
    sprite.print("PREDATOR");
    sprite.setTextColor(HH_PINK, TFT_BLACK);
    sprite.setCursor(tx + 1, 17);
    sprite.print("PREDATOR");
    sprite.setTextColor(TFT_WHITE, TFT_BLACK);
    sprite.setCursor(tx, 16);
    sprite.print("PREDATOR");
    sprite.setFreeFont(NULL);

    // filtered/total (left) and filter name (right), like drawHeader()
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%d/%d", stats->filtered, stats->total);
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(4, 7);
    sprite.print(hdr);
    sprite.setTextColor(HH_VIOLET, TFT_BLACK);
    sprite.setCursor(sprite.width() - (int)strlen(stats->filterName) * 6 - 4, 7);
    sprite.print(stats->filterName);

    // Column headers
    sprite.fillRect(0, BLE_COL_HDR_Y, sprite.width(), BLE_COL_HDR_H, HH_DARK);
    sprite.setTextColor(HH_BLUE, HH_DARK);
    sprite.setCursor(4, BLE_COL_HDR_Y + 2);
    sprite.print("RSSI");
    sprite.setCursor(28, BLE_COL_HDR_Y + 2);
    sprite.print("DEVICE");
    sprite.setCursor(colWhatX, BLE_COL_HDR_Y + 2);
    sprite.print("WHAT");

    if (stats->count == 0) {
        sprite.setTextColor(HH_GUNMETAL, TFT_BLACK);
        const char *msg = "Listening for devices...";
        sprite.setCursor((sprite.width() - (int)strlen(msg) * 6) / 2,
                         BLE_LIST_Y + BLE_LIST_H / 2 - 4);
        sprite.print(msg);
    } else {
        int first = 0;
        if (selected >= visible) first = selected - visible + 1;

        for (int row = 0; row < visible && (first + row) < stats->count; row++) {
            int i = first + row;
            const BleDisplayRow *d = &rows[i];
            int ry = BLE_LIST_Y + row * BLE_ROW_H;
            bool sel = (i == selected);
            uint16_t bg = sel ? HH_DARK : TFT_BLACK;

            if (sel) sprite.fillRect(0, ry, sprite.width(), BLE_ROW_H - 1, HH_DARK);

            uint16_t color;
            if (sel) color = HH_PINK;
            else if (d->tier == 1) color = TFT_RED;
            else if (d->active && d->hasName) color = TFT_WHITE;
            else if (d->active) color = HH_BLUE;
            else if (d->companyKnown) color = HH_VIOLET;
            else if (d->stale) color = HH_GUNMETAL;
            else color = HH_BLUE;

            if (sel) {
                sprite.setTextColor(HH_PINK, bg);
                sprite.setCursor(0, ry + 4);
                sprite.print(">");
            }

            renderBleBars(6, ry + 1, d->rssi);

            sprite.setTextColor(color, bg);
            sprite.setCursor(28, ry + 4);
            sprite.print(d->device);
            sprite.setCursor(colWhatX, ry + 4);
            sprite.print(d->what);

            if (d->tier == 1) {
                sprite.setTextColor(TFT_RED, bg);
                sprite.setCursor(colBangX, ry + 4);
                sprite.print("!");
            }

            if (d->active) {
                sprite.fillCircle(sprite.width() - 6, ry + 8, 2, HH_PINK);
            }
        }
    }

    // Stats line — tier counts + elapsed, like drawDeviceList()'s stats row
    int sy = sprite.height() - BLE_STATS_H - BLE_FOOTER_H;
    sprite.fillRect(0, sy, sprite.width(), BLE_STATS_H, TFT_BLACK);
    sprite.setTextColor(TFT_RED, TFT_BLACK);
    sprite.setCursor(4, sy + 2);
    sprite.printf("RED:%d", stats->red);
    sprite.setTextColor(HH_BLUE, TFT_BLACK);
    sprite.setCursor(64, sy + 2);
    sprite.printf("YEL:%d", stats->yellow);
    sprite.setTextColor(HH_VIOLET, TFT_BLACK);
    sprite.setCursor(130, sy + 2);
    sprite.printf("Total:%d", stats->red + stats->yellow);
    sprite.setCursor(colTimeX, sy + 2);
    sprite.printf("%02d:%02d", (int)(stats->elapsedSec / 60), (int)(stats->elapsedSec % 60));

    // Footer — page indicator + control hints
    int fy = sprite.height() - BLE_FOOTER_H;
    sprite.fillRect(0, fy, sprite.width(), BLE_FOOTER_H, HH_GUNMETAL);
    sprite.setTextColor(HH_BLUE, HH_GUNMETAL);
    sprite.setCursor(4, fy + 2);
#ifdef BOARD_M5STICK_S3
    sprite.print("A+B Filter");
#else
    sprite.print("HOLD=Filter");
#endif
    int pages = (stats->filtered + visible - 1) / visible;
    if (pages < 1) pages = 1;
    int cur = (selected / visible) + 1;
    if (cur > pages) cur = pages;
    char pg[12];
    snprintf(pg, sizeof(pg), "%d/%d", cur, pages);
    sprite.setTextColor(HH_PINK, HH_GUNMETAL);
    sprite.setCursor((sprite.width() - (int)strlen(pg) * 6) / 2, fy + 2);
    sprite.print(pg);
    sprite.setTextColor(HH_BLUE, HH_GUNMETAL);
    sprite.setCursor(sprite.width() - 9 * 6 - 4, fy + 2);
#ifdef BOARD_M5STICK_S3
    sprite.print("Hold B Exit");
#else
    sprite.print("BACK=Exit");
#endif
}

static void uiRenderTaskFn(void *param) {
    (void)param;

#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    // Own bus (SPI3_HOST), never shared with SD — spiSelectDisplay() is a
    // no-op/false on this board (CYD_DISPLAY_ON_SHARED_SPI=0), so the
    // shared-bus arbitration below would never run tft.init() at all.
    tft.init();
    tft.setRotation(1);
#else
    if (spiSelectDisplay()) {
        tft.init();
        tft.setRotation(3);
        spiDeselect();
    }
#endif

    // Backlight PWM (TFT_eSPI's init only drives it plain HIGH). Channel 0
    // matches the CYD build's ledcWrite(0, brightness_level) convention.
    ledcSetup(0, 5000, 8);
    ledcAttachPin(CYD_TFT_BL, 0);

    sprite.setColorDepth(16);
    sprite.createSprite(tft.width(), tft.height());

#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    for (int i = 0; i < CARDPUTER_NUM_MENU_ITEMS; i++) {
        s_mainMenuTitles[i] = CARDPUTER_MAIN_MENU[i].title;
    }
#else
    for (int i = 0; i < NUM_MENU_ITEMS; i++) {
        s_mainMenuTitles[i] = MAIN_MENU[i].title;
    }
#endif

    // Apply whatever the orchestrator configured before this task started.
    {
        DisplaySettings d;
        xSemaphoreTake(s_stateMutex, portMAX_DELAY);
        d = s_disp;
        xSemaphoreGive(s_stateMutex);
        applyDisplaySettings(d);
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ScreenState local;
        DisplaySettings disp;
        xSemaphoreTake(s_stateMutex, portMAX_DELAY);
        local = s_state;
        disp = s_disp;
        if (local.kind == ScreenState::BLE_LIST) {
            s_bleDrawStats = s_bleStats;
            s_bleDrawSelected = s_bleSelected;
            int n = s_bleStats.count;
            if (n > UI_BLE_MAX_ROWS) n = UI_BLE_MAX_ROWS;
            if (n > 0) {
                memcpy(s_bleDrawRows, s_bleRows, n * sizeof(BleDisplayRow));
            }
        }
        if (local.kind == ScreenState::NRF_SNIFFER) {
            s_nrfSniffDrawStats = s_nrfSniffStats;
            s_nrfSniffDrawSelected = s_nrfSniffSelected;
            int n = s_nrfSniffStats.count;
            if (n > UI_NRF_MAX_ROWS) n = UI_NRF_MAX_ROWS;
            if (n > 0) {
                memcpy(s_nrfSniffDrawRows, s_nrfSniffRows,
                       n * sizeof(NrfSniffDisplayRow));
            }
        }
        if (local.kind == ScreenState::MOUSEJACK) {
            s_mjDraw = s_mjDisplay;
        }
        if (local.kind == ScreenState::WIFI_MONITOR) {
            s_wifiMonitorDraw = s_wifiMonitor;
            memcpy(s_wifiHistoryDraw, s_wifiHistory, sizeof(s_wifiHistoryDraw));
        }
        xSemaphoreGive(s_stateMutex);

        applyDisplaySettings(disp);

        if (local.kind == ScreenState::PLACEHOLDER) {
            renderPlaceholder(local.title);
        } else if (local.kind == ScreenState::STATUS) {
            renderStatus(local.title, local.message, local.hint);
        } else if (local.kind == ScreenState::WIFI_STATUS) {
            renderWifiStatus(local.title, local.message, local.hint);
        } else if (local.kind == ScreenState::WIFI_LIST) {
            renderWifiList(local.title, local.items, local.count, local.selected, local.hint);
        } else if (local.kind == ScreenState::WIFI_MONITOR) {
            renderWifiMonitor(s_wifiMonitorDraw, s_wifiHistoryDraw);
        } else if (local.kind == ScreenState::SPECTRUM) {
            renderSpectrum(local.title, local.spectrum, local.specCount);
        } else if (local.kind == ScreenState::NRF_SCANNER) {
            renderNrfScanner(local.spectrum, local.specCount);
        } else if (local.kind == ScreenState::NRF_ANALYZER) {
            renderNrfAnalyzer(local.spectrum, local.specCount, local.lockWifiCh,
                              local.message, local.running);
        } else if (local.kind == ScreenState::NRF_SNIFFER) {
            renderNrfSniffer(s_nrfSniffDrawRows, &s_nrfSniffDrawStats,
                             s_nrfSniffDrawSelected);
        } else if (local.kind == ScreenState::MOUSEJACK) {
            renderMouseJack(&s_mjDraw);
        } else if (local.kind == ScreenState::BLE_LIST) {
            renderBleDevices(s_bleDrawRows, &s_bleDrawStats, s_bleDrawSelected);
        } else if (local.kind == ScreenState::SPLASH) {
            renderSplash();
        } else if (local.kind == ScreenState::INFO) {
            renderInfo(local.title, local.items, local.count);
        } else if (local.kind == ScreenState::LOCK) {
            renderLock(local.message);
        } else if (local.kind == ScreenState::MAIN_MENU) {
            renderMainMenu(local.items, local.count, local.selected);
        } else {
            renderList(local.title, local.items, local.icons, local.count,
                       local.selected, local.hint);
        }

        // Battery indicator overlays the generic screens.
        if (local.kind != ScreenState::BLE_LIST && local.kind != ScreenState::SPLASH &&
            local.kind != ScreenState::LOCK && local.kind != ScreenState::WIFI_STATUS &&
            local.kind != ScreenState::WIFI_LIST && local.kind != ScreenState::WIFI_MONITOR) {
            renderBatteryIndicator();
        }

#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
        // Own bus — see the tft.init() branch above for why this can't go
        // through spiSelectDisplay()/spiDeselect() on this board.
        sprite.pushSprite(0, 0);
#else
        // Panel shares the SD/CC1101/NRF24 bus on this board — see the risk
        // note in ui_render.h.
        if (spiSelectDisplay()) {
            sprite.pushSprite(0, 0);
            spiDeselect();
        }
#endif
    }
}

void uiRenderStart() {
    s_stateMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(uiRenderTaskFn, "ui_render", 4096, NULL, 2, &s_taskHandle, 1);
}

static void setStateAndNotify(const ScreenState &next) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_state = next;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderShowMainMenu(int selectedIndex) {
    ScreenState next;
    next.kind = ScreenState::MAIN_MENU;
    strncpy(next.title, "HaleHound", sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = s_mainMenuTitles;
    next.icons = NULL;
#if defined(BOARD_CARDPUTER_ADV) || defined(BOARD_M5STICK_S3)
    next.count = CARDPUTER_NUM_MENU_ITEMS;
#else
    next.count = NUM_MENU_ITEMS;
#endif
    next.selected = selectedIndex;
    next.message[0] = '\0';
    setStateAndNotify(next);
}

void uiRenderShowSubmenu(const char *title, const char *const *items, int count,
                         int selectedIndex, const char *hint) {
    ScreenState next = {};
    next.kind = ScreenState::SUBMENU;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = items;
    next.icons = NULL;
    next.count = count;
    next.selected = selectedIndex;
    next.message[0] = '\0';
    strlcpy(next.hint, hint ? hint : "", sizeof(next.hint));
    setStateAndNotify(next);
}

void uiRenderShowSubmenuIcons(const char *title, const char *const *items,
                              const unsigned char *const *icons, int count, int selectedIndex) {
    ScreenState next = {};
    next.kind = ScreenState::SUBMENU;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = items;
    next.icons = icons;
    next.count = count;
    next.selected = selectedIndex;
    next.message[0] = '\0';
    setStateAndNotify(next);
}

void uiRenderShowPlaceholder(const char *title) {
    ScreenState next;
    next.kind = ScreenState::PLACEHOLDER;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    next.message[0] = '\0';
    setStateAndNotify(next);
}

void uiRenderShowStatus(const char *title, const char *text, const char *hint) {
    ScreenState next = {};
    next.kind = ScreenState::STATUS;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    strncpy(next.message, text, sizeof(next.message) - 1);
    next.message[sizeof(next.message) - 1] = '\0';
#ifdef BOARD_M5STICK_S3
    strlcpy(next.hint, hint ? hint : "Hold B Back", sizeof(next.hint));
#else
    strlcpy(next.hint, hint ? hint : "SIDE Back", sizeof(next.hint));
#endif
    setStateAndNotify(next);
}

void uiRenderShowWifiStatus(const char *title, const char *text, const char *hint) {
    ScreenState next = {};
    next.kind = ScreenState::WIFI_STATUS;
    strlcpy(next.title, title ? title : "WIFI", sizeof(next.title));
    strlcpy(next.message, text ? text : "", sizeof(next.message));
#ifdef BOARD_M5STICK_S3
    strlcpy(next.hint, hint ? hint : "Hold B Back", sizeof(next.hint));
#else
    strlcpy(next.hint, hint ? hint : "SIDE=Back", sizeof(next.hint));
#endif
    setStateAndNotify(next);
}

void uiRenderShowWifiList(const char *title, const char *const *items, int count,
                          int selectedIndex, const char *hint) {
    ScreenState next = {};
    next.kind = ScreenState::WIFI_LIST;
    strlcpy(next.title, title ? title : "WIFI", sizeof(next.title));
    next.items = items;
    next.count = count;
    next.selected = selectedIndex;
#ifdef BOARD_M5STICK_S3
    strlcpy(next.hint, hint ? hint : "A/B Move | Hold A Select/B Back", sizeof(next.hint));
#else
    strlcpy(next.hint, hint ? hint : "ROT=Select SIDE=Back", sizeof(next.hint));
#endif
    setStateAndNotify(next);
}

void uiRenderShowWifiMonitor(const WifiMonitorMsg *state) {
    if (!state) return;

    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    if (s_state.kind != ScreenState::WIFI_MONITOR) {
        memset(s_wifiHistory, 0, sizeof(s_wifiHistory));
    }
    s_wifiMonitor = *state;
    memmove(s_wifiHistory[0], s_wifiHistory[1], sizeof(s_wifiHistory) - sizeof(s_wifiHistory[0]));
    memcpy(s_wifiHistory[WIFI_WATERFALL_ROWS - 1], state->spectrum,
           sizeof(s_wifiHistory[WIFI_WATERFALL_ROWS - 1]));

    ScreenState next = {};
    next.kind = ScreenState::WIFI_MONITOR;
    strlcpy(next.title, "PKT MONITOR", sizeof(next.title));
    s_state = next;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) xTaskNotifyGive(s_taskHandle);
}

void uiRenderShowSpectrum(const char *title, const uint8_t *bins, int count) {
    ScreenState next;
    next.kind = ScreenState::SPECTRUM;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    next.message[0] = '\0';
    next.specCount = (count > RENDER_MSG_SPECTRUM_BINS) ? RENDER_MSG_SPECTRUM_BINS : count;
    memcpy(next.spectrum, bins, next.specCount);
    setStateAndNotify(next);
}

void uiRenderShowNrfScanner(const uint8_t *bins, int count) {
    ScreenState next;
    next.kind = ScreenState::NRF_SCANNER;
    next.title[0] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    next.message[0] = '\0';
    next.specCount = (count > RENDER_MSG_SPECTRUM_BINS) ? RENDER_MSG_SPECTRUM_BINS : count;
    if (next.specCount > 0 && bins) {
        memcpy(next.spectrum, bins, next.specCount);
    }
    setStateAndNotify(next);
}

void uiRenderShowNrfAnalyzer(const uint8_t *bins, int count, int lockedWifiCh,
                             const char *ssid, bool running) {
    ScreenState next;
    next.kind = ScreenState::NRF_ANALYZER;
    next.title[0] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    next.lockWifiCh = (int8_t)lockedWifiCh;
    next.running = running;
    strncpy(next.message, ssid ? ssid : "", sizeof(next.message) - 1);
    next.message[sizeof(next.message) - 1] = '\0';
    next.specCount = (count > RENDER_MSG_SPECTRUM_BINS) ? RENDER_MSG_SPECTRUM_BINS : count;
    if (next.specCount > 0 && bins) {
        memcpy(next.spectrum, bins, next.specCount);
    }
    setStateAndNotify(next);
}

void uiRenderShowNrfSniffer(const NrfSniffDisplayRow *rows,
                            const NrfSniffDisplayStats *stats,
                            int selectedIndex) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_state.kind = ScreenState::NRF_SNIFFER;
    s_state.icons = NULL;
    s_nrfSniffStats = *stats;
    s_nrfSniffSelected = selectedIndex;
    int n = stats->count;
    if (n > UI_NRF_MAX_ROWS) n = UI_NRF_MAX_ROWS;
    if (n < 0) n = 0;
    if (n > 0 && rows) {
        memcpy(s_nrfSniffRows, rows, n * sizeof(NrfSniffDisplayRow));
    }
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderShowMouseJack(const MouseJackDisplay *st) {
    if (!st) return;
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_state.kind = ScreenState::MOUSEJACK;
    s_state.icons = NULL;
    s_mjDisplay = *st;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderShowBleDevices(const BleDisplayRow *rows, const BleDisplayStats *stats,
                            int selectedIndex) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_state.kind = ScreenState::BLE_LIST;
    s_state.icons = NULL;
    s_bleStats = *stats;
    s_bleSelected = selectedIndex;
    int n = stats->count;
    if (n > UI_BLE_MAX_ROWS) n = UI_BLE_MAX_ROWS;
    if (n < 0) n = 0;
    if (n > 0 && rows) {
        memcpy(s_bleRows, rows, n * sizeof(BleDisplayRow));
    }
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderApplySettings(uint8_t brightness, uint8_t rotationIndex,
                           bool swapColors, bool invert, uint8_t colorMode) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_disp.brightness = brightness;
    s_disp.rotationIndex = rotationIndex;
    s_disp.swapColors = swapColors;
    s_disp.invert = invert;
    s_disp.colorMode = colorMode;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderSetBrightness(uint8_t brightness) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_disp.brightness = brightness;
    s_disp.backlightOn = true;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderSetBacklight(bool on) {
    xSemaphoreTake(s_stateMutex, portMAX_DELAY);
    s_disp.backlightOn = on;
    xSemaphoreGive(s_stateMutex);
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}

void uiRenderShowSplash() {
    ScreenState next;
    next.kind = ScreenState::SPLASH;
    next.title[0] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    next.message[0] = '\0';
    setStateAndNotify(next);
}

void uiRenderShowInfo(const char *title, const char *const *lines, int count) {
    ScreenState next;
    next.kind = ScreenState::INFO;
    strncpy(next.title, title, sizeof(next.title) - 1);
    next.title[sizeof(next.title) - 1] = '\0';
    next.items = lines;
    next.icons = NULL;
    next.count = count;
    next.selected = 0;
    next.message[0] = '\0';
    setStateAndNotify(next);
}

void uiRenderShowLock(const char *mask) {
    ScreenState next;
    next.kind = ScreenState::LOCK;
    next.title[0] = '\0';
    next.items = NULL;
    next.icons = NULL;
    next.count = 0;
    next.selected = 0;
    strncpy(next.message, mask, sizeof(next.message) - 1);
    next.message[sizeof(next.message) - 1] = '\0';
    setStateAndNotify(next);
}

void uiRenderRefresh() {
    if (s_taskHandle) {
        xTaskNotifyGive(s_taskHandle);
    }
}
