// ═══════════════════════════════════════════════════════════════════════════
// HaleHound T-Embed — Settings Persistence (implementation)
// Created: 2026-09-18 (T-Embed CC1101 port, UI parity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "settings_manager.h"
#include <Preferences.h>
#include "cyd_config.h"
#if !defined(BOARD_CARDPUTER_ADV) && !defined(BOARD_M5STICK_S3)
// CC1101/NRF24 tuning are out of scope on Cardputer ADV (no on-board
// radios for either this milestone — board_cardputer_adv.h) — the
// TEmbedSettings fields below still exist on every board (harmless unused
// bytes elsewhere) but setDefaults()/settingsManagerSetup() only reference
// their option-table COUNT constants where this board actually has the
// modules that define them.
#include "cc1101_tuning.h"
#include "nrf24_tuning.h"
#endif

// "TE" -> "T1" -> "T2": bumped again for the v3.9.x Phase 7 TEmbedSettings
// growth (NRF24 tuning fields) — settingsManagerSetup()'s exact-size-match
// load check means this intentionally resets every setting to defaults
// once on the first boot after upgrading (documented existing convention:
// bump on any layout change, no migration path — same tradeoff this file
// already made for every prior field addition, Phase 6's included).
#define SETTINGS_MAGIC 0x5432   // bump if TEmbedSettings layout changes

TEmbedSettings g_settings;

static Preferences s_prefs;
static bool s_prefsOpen = false;

const char *const SETTINGS_TIMEOUT_LABELS[] = {"Off", "15 sec", "30 sec", "1 min", "2 min", "5 min"};
const int SETTINGS_TIMEOUT_VALUES[] = {0, 15, 30, 60, 120, 300};
const int SETTINGS_TIMEOUT_COUNT = 6;

const char *const SETTINGS_COLOR_MODE_LABELS[] = {"Default", "Colorblind", "High Contrast"};
const int SETTINGS_COLOR_MODE_COUNT = 3;

const char *const SETTINGS_ROTATION_LABELS[] = {"Normal", "Flipped"};
const int SETTINGS_ROTATION_COUNT = 2;

static void setDefaults() {
    g_settings.magic = SETTINGS_MAGIC;
    g_settings.brightness = 255;
    g_settings.screenTimeout = 60;
    g_settings.colorSwap = 0;
    g_settings.displayInverted = 0;
    g_settings.colorMode = 0;
    g_settings.rotationIndex = 0;
    g_settings.pinEnabled = 0;
    g_settings.devicePin = 0;

#if !defined(BOARD_CARDPUTER_ADV) && !defined(BOARD_M5STICK_S3)
    // v3.9.x Phase 6 — live tuning off by default (every module behaves
    // exactly as it did before this phase); indices point at each table's
    // "matches what the code already hardcoded" entry, so turning tuning
    // ON for the first time changes nothing until the user actually
    // adjusts a row.
    g_settings.cc1101TuningEnabled = 0;
    g_settings.cc1101ModIndex = 2;        // ASK/OOK
    g_settings.cc1101RxBwIndex = 4;       // 270kHz
    g_settings.cc1101DRateIndex = 2;      // 3.79k
    g_settings.cc1101AgcIndex = 0;        // Max Gain (0x03 — the existing hardcoded AGCCTRL2)
    g_settings.cc1101FreqOffsetIndex = 4; // +0kHz

    // v3.9.x Phase 7 — same "off by default, index matches what the code
    // already hardcoded" convention as Phase 6's CC1101 defaults above.
    g_settings.nrf24TuningEnabled = 0;
    g_settings.nrf24PaIndex = 3;         // Max (0dBm) — every existing TX caller's hardcoded default
    g_settings.nrf24DwellScaleIndex = 2; // 1x
#endif
}

void settingsManagerSetup() {
    s_prefsOpen = s_prefs.begin("halehound", false);

    bool loaded = false;
    if (s_prefsOpen) {
        size_t n = s_prefs.getBytesLength("cfg");
        if (n == sizeof(TEmbedSettings)) {
            s_prefs.getBytes("cfg", &g_settings, sizeof(g_settings));
            loaded = (g_settings.magic == SETTINGS_MAGIC);
        }
    }

    if (!loaded) {
        setDefaults();
        settingsSave();
    }

    // Sanitize anything out of range (also covers a corrupted-but-magic blob)
    if (g_settings.brightness < 16) g_settings.brightness = 16;
    if (g_settings.screenTimeout > 3600) g_settings.screenTimeout = 60;
    if (g_settings.colorMode >= SETTINGS_COLOR_MODE_COUNT) g_settings.colorMode = 0;
    if (g_settings.rotationIndex >= SETTINGS_ROTATION_COUNT) g_settings.rotationIndex = 0;
    if (g_settings.devicePin > 9999) {
        g_settings.devicePin = 0;
        g_settings.pinEnabled = 0;
    }
#if !defined(BOARD_CARDPUTER_ADV) && !defined(BOARD_M5STICK_S3)
    if (g_settings.cc1101ModIndex >= CC1101_TUNING_MOD_COUNT) g_settings.cc1101ModIndex = 2;
    if (g_settings.cc1101RxBwIndex >= CC1101_TUNING_RXBW_COUNT) g_settings.cc1101RxBwIndex = 4;
    if (g_settings.cc1101DRateIndex >= CC1101_TUNING_DRATE_COUNT) g_settings.cc1101DRateIndex = 2;
    if (g_settings.cc1101AgcIndex >= CC1101_TUNING_AGC_COUNT) g_settings.cc1101AgcIndex = 0;
    if (g_settings.cc1101FreqOffsetIndex >= CC1101_TUNING_OFFSET_COUNT) g_settings.cc1101FreqOffsetIndex = 4;
    if (g_settings.nrf24PaIndex >= NRF24_TUNING_PA_COUNT) g_settings.nrf24PaIndex = 3;
    if (g_settings.nrf24DwellScaleIndex >= NRF24_TUNING_DWELL_COUNT) g_settings.nrf24DwellScaleIndex = 2;
#endif

    #if CYD_DEBUG
    Serial.printf("[SETTINGS] loaded: bright=%d timeout=%d swap=%d invert=%d mode=%d rot=%d pin=%d\n",
                  g_settings.brightness, g_settings.screenTimeout, g_settings.colorSwap,
                  g_settings.displayInverted, g_settings.colorMode, g_settings.rotationIndex,
                  g_settings.pinEnabled);
    #endif
}

void settingsSave() {
    if (!s_prefsOpen) return;
    s_prefs.putBytes("cfg", &g_settings, sizeof(g_settings));

    #if CYD_DEBUG
    Serial.println("[SETTINGS] saved");
    #endif
}
