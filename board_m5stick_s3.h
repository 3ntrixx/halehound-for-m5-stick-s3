#ifndef BOARD_M5STICK_S3_H
#define BOARD_M5STICK_S3_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Board HAL — M5Stack StickS3 (K150, ESP32-S3)
// Selected by build flag -DBOARD_M5STICK_S3=1 (see platformio.ini)
// Created: 2026-10-04 (StickS3 port, Stage 1-3 — board bring-up + buttons)
//
// Pin source: PORT_M5StickS3_WiFi-BLE_Outline.md §1/§3, itself sourced from
// M5Stack's own StickS3 documentation (docs.m5stack.com/en/core/StickS3).
// NOT hardware-verified in this pass — see the outline's bring-up milestone
// (repeatable boot, correct display orientation/colors, both buttons
// responsive, verified flash/PSRAM sizes) before trusting any pin here.
//
// Scope this pass is on-chip 2.4 GHz WiFi + BLE only. It shares the
// Cardputer's 240x135 menu layout and adds StickS3 SIGINT entries backed by
// LittleFS. CC1101/NRF24/RFID/GPS/IR/audio/IMU stay out of scope — no
// on-board radio for any of those on this SKU, and this board HAL defines
// no pins for them.
//
// This is a separate, independent board HAL from board_cardputer_adv.h and
// board_t_embed_cc1101.h — no file in either of those ports is touched by
// this one.
// ═══════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────
// FIRMWARE VERSION / EDITION
// ─────────────────────────────────────────────────────────────────────────
#define FW_EDITION      "StickS3 Edition"
#define FW_DEVICE       "HaleHound-StickS3"
#define CYD_BOARD_NAME  "M5Stack StickS3"

// ─────────────────────────────────────────────────────────────────────────
// POWER — M5PM1 handles the physical power button. boardPowerSetup() enables
// its PYG2 L3B rail before display initialization; GPIO38 then controls
// backlight brightness. No ESP32 power-hold GPIO is needed.
// ─────────────────────────────────────────────────────────────────────────
#define BOARD_POWER_ON_PIN  -1

// ─────────────────────────────────────────────────────────────────────────
// DISPLAY — ST7789P3, native panel 135x240, run at setRotation(1) for a
// 240x135 landscape UI (same native/landscape pair as the Cardputer ADV's
// ST7789V2 — see board_cardputer_adv.h's CGRAM-offset note; verify this
// panel's own colstart/rowstart on hardware rather than assuming the same
// 52/40 pair carries over from a different ST7789 variant). Own bus — not
// shared with any radio or storage pins on this board.
// ─────────────────────────────────────────────────────────────────────────
#define CYD_SCREEN_WIDTH    240
#define CYD_SCREEN_HEIGHT   135
#define CYD_TFT_MOSI         39
#define CYD_TFT_SCLK         40
#define CYD_TFT_DC           45
#define CYD_TFT_CS           41
#define CYD_TFT_RST          21
#define CYD_TFT_BL           38
#define CYD_TFT_MISO         -1   // 3-wire bus, no MISO

// ─────────────────────────────────────────────────────────────────────────
// TOUCH — none on this board (two-button navigation, see BUTTONS below).
// ─────────────────────────────────────────────────────────────────────────
// (no touch pins — see CYD_HAS_TOUCH feature flag below)

// ─────────────────────────────────────────────────────────────────────────
// ROTARY ENCODER — none on this board; kept defined-to-0 below so shared
// code guarding on CYD_HAS_ENCODER compiles the same way it does for the
// T-Embed/CYD builds. Navigation comes from the two buttons instead
// (button_input.cpp decodes gestures into the same five InputEvents —
// see its header for the mapping table).
// ─────────────────────────────────────────────────────────────────────────
// (no encoder pins — see CYD_HAS_ENCODER feature flag below)

// ─────────────────────────────────────────────────────────────────────────
// BUTTONS — two application buttons. Outline's suggested gesture mapping
// (button_input.h has the full rationale):
//   A short -> Next, B short -> Previous, A long -> Select,
//   B long -> Back/stop, A+B chord -> secondary action.
// ─────────────────────────────────────────────────────────────────────────
#define BOARD_BTN_A_PIN      11
#define BOARD_BTN_B_PIN      12

// No BOOT-button UI convention on this board (GPIO0 is the strap/download
// pin, not a user button in this firmware — same reasoning as the
// Cardputer ADV's board_cardputer_adv.h).
#define BOOT_BUTTON_USABLE    0
#define IS_BOOT_PRESSED() (false)

// ─────────────────────────────────────────────────────────────────────────
// INTERNAL I2C BUS — M5PM1 power-management IC. battery_gauge.cpp does not
// yet have a verified M5PM1 register map (see its BOARD_M5STICK_S3 branch);
// the bus is brought up regardless since later I2C peripherals would share
// it and i2c_manager.cpp's single mutex needs the pins defined either way.
// ─────────────────────────────────────────────────────────────────────────
#define BOARD_I2C_SDA         47
#define BOARD_I2C_SCL         48

// ─────────────────────────────────────────────────────────────────────────
// STORAGE — no microSD slot. storage_manager.cpp uses LittleFS on this
// board. The SD and SPI pin macros are unused compatibility placeholders.
// ─────────────────────────────────────────────────────────────────────────
#define SD_CS                -1
#define SD_SCK               -1
#define SD_MOSI              -1
#define SD_MISO              -1
#define VSPI_SCK             SD_SCK
#define VSPI_MOSI            SD_MOSI
#define VSPI_MISO            SD_MISO
#define RADIO_SPI_SCK         SD_SCK
#define RADIO_SPI_MOSI        SD_MOSI
#define RADIO_SPI_MISO        SD_MISO

// ─────────────────────────────────────────────────────────────────────────
// BATTERY — M5PM1 PMIC over the internal I2C bus above. No dedicated
// fuel-gauge/SOC register on this PMIC (CYD_HAS_FUELGAUGE stays 0 — that
// flag gates the T-Embed's BQ27220 driver specifically), so
// battery_gauge.cpp's BOARD_M5STICK_S3 branch reads raw VBAT and
// interpolates a LiPo discharge curve instead. Still an approximation
// under radio-TX load — see that file's header comment — not hardware-
// verified against a real StickS3 yet.
// ─────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
// DEBUG — verbose [SUBSYS] Serial prints during bring-up.
// ─────────────────────────────────────────────────────────────────────────
#define CYD_DEBUG              1
#define CYD_DEBUG_BAUD    115200

// ─────────────────────────────────────────────────────────────────────────
// FEATURE FLAGS — WiFi/BLE on-chip only this milestone. Everything needing
// an external radio module (CC1101/nRF24/PN532) or hardware this board
// doesn't have (GPS, RTC, encoder, touch, speaker, SD) stays 0.
// ─────────────────────────────────────────────────────────────────────────
#define CYD_HAS_CC1101         0
#define CYD_HAS_NRF24          0
#define CYD_HAS_GPS            0
#define CYD_HAS_SDCARD         0   // onboard LittleFS, no microSD slot
#define CYD_HAS_RGB_LED        0
#define CYD_HAS_WS2812         0
#define CYD_HAS_SPEAKER        0
#define CYD_HAS_PCF8574        0
#define CYD_HAS_PN532          0
#define CYD_HAS_TOUCH          0
#define CYD_HAS_ENCODER        0
#define CYD_HAS_KEYBOARD       0
#define CYD_HAS_BUTTONS        1   // two-button gesture nav — see button_input.h
#define CYD_HAS_IR             0
#define CYD_HAS_MIC            0
#define CYD_HAS_FUELGAUGE      0   // no coulomb-counting fuel gauge on M5PM1
#define CYD_HAS_BATTERY        1   // M5PM1 VBAT read — see battery_gauge.cpp
#define CYD_HAS_AMP            0
#define CYD_HAS_I2C_BUS        1   // internal bus for the M5PM1 PMIC

// Display is on its OWN bus — never shared with SD/radios (there are none).
#define CYD_DISPLAY_ON_SHARED_SPI 0

#endif // BOARD_M5STICK_S3_H
