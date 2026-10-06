#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound T-Embed — Settings Persistence
// Created: 2026-09-18 (T-Embed CC1101 port, UI parity pass)
//
// CYD parity with utils.cpp's Settings struct (EEPROM there), stored in NVS
// via Preferences here. Only the fields that apply to this board are kept —
// no touch calibration, no EEPROM map, no PA module flag (no PA on-board).
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>

struct TEmbedSettings {
    uint16_t magic;
    uint8_t brightness;      // 0-255 (LEDC duty on CYD_TFT_BL)
    uint16_t screenTimeout;  // seconds, 0 = off
    uint8_t colorSwap;       // 0 = BGR (default), 1 = RGB
    uint8_t displayInverted; // 0 = normal, 1 = inverted
    uint8_t colorMode;       // 0 = Default, 1 = Colorblind, 2 = High Contrast
    uint8_t rotationIndex;   // 0 = Normal (tft rot 3), 1 = Flipped (tft rot 1)
    uint8_t pinEnabled;      // 0 = disabled, 1 = PIN lock active
    uint16_t devicePin;      // 4-digit PIN stored as 0-9999

    // v3.9.x Phase 6 (HALEHOUND_V3.9X_PORT_PLAN.md) — persisted CC1101 live
    // tuning override, indices into cc1101_tuning.h's option tables (never
    // raw floats/registers here, so a stray NVS bit flip can't produce an
    // out-of-range value settingsManagerSetup()'s sanitize pass doesn't
    // already clamp the same way it clamps colorMode/rotationIndex).
    uint8_t cc1101TuningEnabled;  // 0 = every module uses its own preset/default, 1 = override below applies
    uint8_t cc1101ModIndex;       // index into CC1101_TUNING_MOD_LABELS/VALUES
    uint8_t cc1101RxBwIndex;      // index into CC1101_TUNING_RXBW_LABELS/VALUES
    uint8_t cc1101DRateIndex;     // index into CC1101_TUNING_DRATE_LABELS/VALUES
    uint8_t cc1101AgcIndex;       // index into CC1101_TUNING_AGC_LABELS/VALUES
    uint8_t cc1101FreqOffsetIndex; // index into CC1101_TUNING_OFFSET_LABELS/VALUES

    // v3.9.x Phase 7 — persisted NRF24 live tuning override (nrf24_tuning.h).
    uint8_t nrf24TuningEnabled;    // 0 = every module uses its own default, 1 = override below applies
    uint8_t nrf24PaIndex;          // index into NRF24_TUNING_PA_LABELS/VALUES
    uint8_t nrf24DwellScaleIndex;  // index into NRF24_TUNING_DWELL_LABELS/VALUES
};

// Loads settings from NVS, writing defaults on first run/corruption.
void settingsManagerSetup();

// Persists the current settings.
void settingsSave();

extern TEmbedSettings g_settings;

// Option tables shared by the Settings screens.
extern const char *const SETTINGS_TIMEOUT_LABELS[];
extern const int SETTINGS_TIMEOUT_VALUES[];
extern const int SETTINGS_TIMEOUT_COUNT;
extern const char *const SETTINGS_COLOR_MODE_LABELS[];
extern const int SETTINGS_COLOR_MODE_COUNT;
extern const char *const SETTINGS_ROTATION_LABELS[];
extern const int SETTINGS_ROTATION_COUNT;

#endif // SETTINGS_MANAGER_H
