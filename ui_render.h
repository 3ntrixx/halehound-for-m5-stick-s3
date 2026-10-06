#ifndef UI_RENDER_H
#define UI_RENDER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound UI Render Task
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 3 / plan §4.2)
//
// Sole writer to the ST7789 (plan §4.2 task table). Owns TFT_eSPI and a
// full-frame PSRAM-backed sprite (plan §5.2 — 320x170x2B here since the
// panel runs in landscape, ~106KB, trivial against 8MB PSRAM). The
// orchestrator (Arduino loop(), core 1) calls the uiRenderShow*() setters
// below to describe what should be on screen; this task wakes on a task
// notification, redraws the sprite, and pushes it — all bracketed by
// spiSelectDisplay()/spiDeselect() since the panel shares the SD/CC1101/
// NRF24 SPI bus on this board (board_t_embed_cc1101.h).
//
// UPDATE (Phase 5 hardware debugging): TFT_eSPI must run on HSPI on this
// board (see User_Setup_TEmbed.h for the IDF REG_SPI_BASE numbering bug that
// makes its FSPI path panic in tft.init()). TFT_eSPI therefore owns the
// shared bus through its own global `SPIClass(HSPI)` instance in
// Processors/TFT_eSPI_ESP32_S3.c — and that instance is THE shared bus:
// spi_manager.cpp exposes it as spiBus(), and storage_manager / the vendored
// CC1101 driver / the SubGHz presence probes all talk through it. The
// framework's global `SPI` object (FSPI/SPI2) is a DIFFERENT peripheral and
// is never used on this board. (That also resolved the old link-time `spi`
// symbol collision: the vendored driver's `static bool spi` flag was renamed
// and the driver now uses TFT_eSPI's instance directly.)
//
// Remaining open question: subghz_scan.cpp's worker takes spi_manager's
// mutex only around each short burst of CC1101 activity (never for the
// whole feature's lifetime) specifically so this task can still get the
// bus between bursts — verify on hardware that the display keeps
// redrawing smoothly (no visible stall/corruption) while a CC1101 sweep
// is running continuously alongside it.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "messaging.h"

// Starts the ui_render task. Call once from setup(). Initializes TFT_eSPI
// and the PSRAM sprite from within the task itself (so tft/sprite are only
// ever touched from this one task, per the "sole writer" rule).
void uiRenderStart();

// Show the top-level menu, highlighting `selectedIndex`.
void uiRenderShowMainMenu(int selectedIndex);

// Show a submenu list under `title`, highlighting `selectedIndex`.
void uiRenderShowSubmenu(const char *title, const char *const *items, int count,
                         int selectedIndex, const char *hint = nullptr);

// Same, but with a 16x16 icon per row (CYD submenu style: icon at the left,
// "| " then the label). Pass NULL icons for the plain text list.
void uiRenderShowSubmenuIcons(const char *title, const char *const *items,
                              const unsigned char *const *icons, int count, int selectedIndex);

// Show a placeholder screen for a leaf item that isn't ported yet
// (everything is a placeholder until Phase 4+ wires in real features).
void uiRenderShowPlaceholder(const char *title);

// Show a single live status line under `title` (a running feature's
// telemetry — e.g. packet monitor's "CH 6  packets: 1234").
void uiRenderShowStatus(const char *title, const char *text,
                        const char *hint = nullptr);

// WiFi-family screens use the original HaleHound palette and compact
// encoder hints rather than the generic bring-up status/list pages.
void uiRenderShowWifiStatus(const char *title, const char *text, const char *hint);
void uiRenderShowWifiList(const char *title, const char *const *items, int count,
                          int selectedIndex, const char *hint);
void uiRenderShowWifiMonitor(const WifiMonitorMsg *state);

// Show a live bar-chart spectrum frame under `title` — `bins` holds
// `count` levels (0-125, RENDER_MSG_SPECTRUM_FRAME's payload.spectrum —
// see messaging.h) rendered as heat-colored bars with peak hold, dBm axis,
// and markers for the three calibrated CC1101 receive bands.
void uiRenderShowSpectrum(const char *title, const uint8_t *bins, int count);

// 2.4GHz NRF24 scanner screen — port of the CYD build's Scanner UI
// (nrf24_attacks.cpp's drawScannerFrame()/drawBarGraph()): 85-channel bar
// graph over 2400-2484MHz with WiFi channel markers, PEAK readout and the
// 8-skull signal meter. `bins` holds `count` smoothed levels (0-125).
void uiRenderShowNrfScanner(const uint8_t *bins, int count);

// 2.4GHz NRF24 spectrum analyzer screen — port of the CYD build's Analyzer
// UI (nrf24_attacks.cpp's drawSpectrum()/drawSkullWaterfall()/
// drawStatusArea()): 85-channel spectrum plus a 3-row skull waterfall,
// WiFi markers (or a single AP-lock marker when lockedWifiCh > 0) and a
// peak/run-status footer. `running` drives the RUN/HOLD indicator.
void uiRenderShowNrfAnalyzer(const uint8_t *bins, int count, int lockedWifiCh,
                             const char *ssid, bool running);

// ─────────────────────────────────────────────────────────────────────────
// NRF24 promiscuous sniffer device table — port of the CYD build's
// NrfSniffer screen (nrf24_attacks.cpp's drawSnHeader()/drawPacketList()):
// ADDR / CH / TYPE / # table with hit counts and RPD dots, header stats
// (CH / DEV / FRAMES) and the 8-skull RX meter. The original's detail/hex
// view is deferred to the MouseJack port, since its "USE FOR MOUSEJACK"
// handoff is the only consumer.
// ─────────────────────────────────────────────────────────────────────────
#define UI_NRF_MAX_ROWS 16

struct NrfSniffDisplayRow {
    char addr[18];   // "AA:BB:CC:DD:EE"
    char type[5];    // "WAKE"/"KEY "/"MOUS"/...
    uint8_t channel;
    uint16_t hits;
    bool rpd;
};

struct NrfSniffDisplayStats {
    int count;
    uint32_t totalFrames;
    int currentCh;   // -1 = hopping (ALL)
    bool scanning;
};

void uiRenderShowNrfSniffer(const NrfSniffDisplayRow *rows,
                            const NrfSniffDisplayStats *stats,
                            int selectedIndex);

// ─────────────────────────────────────────────────────────────────────────
// NRF24 MouseJack — port of nrf24_attacks.cpp's MouseJack screen: header
// status (STANDBY/SCANNING/INJECTING/COMPLETE), target frame (device type,
// pipe address, channel, test result), payload line, scan/inject progress
// area and the encoder-native action bar that replaces the original's touch
// icon row (SCAN / TEST / INJECT / PAYLOAD). See mousejack.h.
// ─────────────────────────────────────────────────────────────────────────

struct MouseJackDisplay {
    bool hasTarget;
    uint8_t addr[5];
    uint8_t channel;
    uint8_t deviceType;      // 0xC1=keyboard, 0xC2=mouse, 0x0F=HID

    bool scanning;
    bool injecting;
    int scanProgress;        // 0-100
    int scanPass;            // 1-3
    int scanCh;

    int payloadIndex;
    char payloadName[16];

    int keystrokesSent;
    int keystrokesTotal;
    int packetsOk;
    int packetsFail;

    bool testValid;
    bool testOk;

    int selectedAction;      // 0=SCAN 1=TEST 2=INJECT 3=PAYLOAD
};

void uiRenderShowMouseJack(const MouseJackDisplay *st);

// ─────────────────────────────────────────────────────────────────────────
// BLE Predator device list — screen-for-screen port of the CYD build's
// drawHeader()/drawColumnHeaders()/drawDeviceList() layout: glitch
// "PREDATOR" title, RSSI-bar / DEVICE / WHAT columns, tier-aware row
// colors, RED "!" marker, active dot, stats line (RED/YEL/Total/elapsed)
// and a page-indicator footer. See bluetooth_attacks.cpp's BlePredator.
// ─────────────────────────────────────────────────────────────────────────

#define UI_BLE_MAX_ROWS 62   // matches BP_MAX_DEVICES

struct BleDisplayRow {
    char device[19];   // name, or "XX:XX:XX" when unnamed
    char what[14];     // appearance detail / company / "Named" / "???"
    int8_t rssi;
    uint8_t tier;      // 0 = yellow, 1 = red (SKIP rows never reach here)
    bool hasName;
    bool companyKnown;
    bool active;       // original's active dot: >5 frames, seen <10s ago
    bool stale;        // original's age>30s gunmetal color
};

struct BleDisplayStats {
    int count;         // filtered rows in `rows`
    int filtered;      // devices passing the current filter
    int red;
    int yellow;
    int total;         // tracked devices, all tiers
    uint32_t elapsedSec;
    char filterName[8]; // "ALL"/"NAMED"/"STRONG"/"HIGH"
};

void uiRenderShowBleDevices(const BleDisplayRow *rows, const BleDisplayStats *stats,
                            int selectedIndex);

// ─────────────────────────────────────────────────────────────────────────
// Display settings (Settings screens + boot application)
// ─────────────────────────────────────────────────────────────────────────

// Applies persisted display settings — the render task (sole owner of
// TFT_eSPI/backlight) applies them on its next wake. rotationIndex: 0 =
// Normal (landscape), 1 = Flipped; colorMode: 0/1/2 matching
// settings_manager.h's labels.
void uiRenderApplySettings(uint8_t brightness, uint8_t rotationIndex,
                           bool swapColors, bool invert, uint8_t colorMode);

// Live backlight update (0-255) for the Brightness screen.
void uiRenderSetBrightness(uint8_t brightness);

// Backlight on/off for the screen-timeout idle blank.
void uiRenderSetBacklight(bool on);

// Boot splash (glitch HALEHOUND + board name/edition).
void uiRenderShowSplash();

// Full-page info screen (About / Device Info): glitch title + one line per
// entry. The caller's `lines` array must stay valid while shown (pass a
// static buffer).
void uiRenderShowInfo(const char *title, const char *const *lines, int count);

// PIN lock screen: `mask` is the partially-entered PIN (e.g. "12__").
void uiRenderShowLock(const char *mask);

// Re-notifies the render task to redraw the current screen without changing
// its state — used when the battery percentage changes under the indicator.
void uiRenderRefresh();

#endif // UI_RENDER_H
