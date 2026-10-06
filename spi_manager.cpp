// ═══════════════════════════════════════════════════════════════════════════
// HaleHound SPI Bus Manager Implementation
// Created: 2026-02-06. Promoted to a real FreeRTOS recursive mutex + DISPLAY
// device: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §6.3).
// ═══════════════════════════════════════════════════════════════════════════

#include "spi_manager.h"

// ═══════════════════════════════════════════════════════════════════════════
// SHARED BUS INSTANCE
// ═══════════════════════════════════════════════════════════════════════════

#ifdef BOARD_T_EMBED_CC1101
// TFT_eSPI's own global SPIClass(HSPI) instance, defined in
// Processors/TFT_eSPI_ESP32_S3.c (external linkage — this is the same global
// symbol the vendored CC1101 driver's `static bool spi` was renamed to avoid
// colliding with). On this board the display, CC1101, NRF24, and SD all sit
// on one physical bus, so TFT_eSPI's instance is the one that must be used
// by everyone — see User_Setup_TEmbed.h.
extern SPIClass spi;
SPIClass& spiBus() { return spi; }
#elif defined(BOARD_CARDPUTER_ADV)
// Cardputer ADV: SD lives on its own independent bus (FSPI/SPI2_HOST),
// never shared with the display (TFT_eSPI's own SPI3_HOST/HSPI instance —
// see User_Setup_Cardputer.h) the way the T-Embed's bus is. A dedicated
// instance, explicitly FSPI, keeps this unambiguous rather than relying on
// whatever the framework's default global SPI happens to map to on S3.
static SPIClass s_cardputerSdSpi(FSPI);
SPIClass& spiBus() { return s_cardputerSdSpi; }
#elif defined(BOARD_M5STICK_S3)
// M5Stick S3: no microSD slot and no shared-bus radio modules at all this
// pass (board_m5stick_s3.h — CYD_HAS_SDCARD/CC1101/NRF24/PN532 are all 0).
// The display is on its own bus too (CYD_DISPLAY_ON_SHARED_SPI=0), so
// nothing in this firmware selects a device through spiBus() on this board.
// LittleFS uses the internal flash controller; the shared SPI peripheral is
// left uninitialized so its default pins cannot interfere with the buttons.
SPIClass& spiBus() { return SPI; }
#else
SPIClass& spiBus() { return SPI; }
#endif

// ═══════════════════════════════════════════════════════════════════════════
// INTERNAL STATE
// ═══════════════════════════════════════════════════════════════════════════

static SPIDevice currentDevice = SPI_DEVICE_NONE;
static SemaphoreHandle_t s_spiMutex = NULL;
static volatile uint32_t s_explicitLockDepth = 0;   // spiLock()/spiUnlock() only
static bool initialized = false;

// ═══════════════════════════════════════════════════════════════════════════
// CS PIN CONTROL
// ═══════════════════════════════════════════════════════════════════════════

// Set all CS pins HIGH (deselect all devices)
static void deselectAllCS() {
    #if CYD_HAS_SDCARD
    digitalWrite(SD_CS, HIGH);
    #endif

    #if CYD_HAS_CC1101
    digitalWrite(CC1101_CS, HIGH);
    #endif

    #if CYD_HAS_NRF24
    digitalWrite(NRF24_CSN, HIGH);
    #endif

    #if CYD_HAS_PN532
    digitalWrite(PN532_CS, HIGH);
    #endif

    #if CYD_DISPLAY_ON_SHARED_SPI
    digitalWrite(CYD_TFT_CS, HIGH);
    #endif
}

// Select a specific CS pin (pull LOW)
static void selectCS(SPIDevice device) {
    switch (device) {
        case SPI_DEVICE_SD:
            #if CYD_HAS_SDCARD
            digitalWrite(SD_CS, LOW);
            #endif
            break;

        case SPI_DEVICE_CC1101:
            #if CYD_HAS_CC1101
            digitalWrite(CC1101_CS, LOW);
            #endif
            break;

        case SPI_DEVICE_NRF24:
            #if CYD_HAS_NRF24
            digitalWrite(NRF24_CSN, LOW);
            #endif
            break;

        case SPI_DEVICE_PN532:
            #if CYD_HAS_PN532
            digitalWrite(PN532_CS, LOW);
            #endif
            break;

        case SPI_DEVICE_DISPLAY:
            #if CYD_DISPLAY_ON_SHARED_SPI
            digitalWrite(CYD_TFT_CS, LOW);
            #endif
            break;

        case SPI_DEVICE_NONE:
        default:
            // All deselected
            break;
    }
}

static bool deviceEnabled(SPIDevice device) {
    switch (device) {
        case SPI_DEVICE_SD:      return CYD_HAS_SDCARD;
        case SPI_DEVICE_CC1101:  return CYD_HAS_CC1101;
        case SPI_DEVICE_NRF24:   return CYD_HAS_NRF24;
        case SPI_DEVICE_PN532:   return CYD_HAS_PN532;
        case SPI_DEVICE_DISPLAY: return CYD_DISPLAY_ON_SHARED_SPI;
        case SPI_DEVICE_NONE:    return true;
        default:                 return false;
    }
}

static const char* deviceName(SPIDevice device) {
    switch (device) {
        case SPI_DEVICE_NONE:    return "NONE";
        case SPI_DEVICE_SD:      return "SD";
        case SPI_DEVICE_CC1101:  return "CC1101";
        case SPI_DEVICE_NRF24:   return "NRF24";
        case SPI_DEVICE_PN532:   return "PN532";
        case SPI_DEVICE_DISPLAY: return "DISPLAY";
        default:                 return "?";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

void spiManagerSetup() {
    if (initialized) {
        return;  // Already initialized
    }

    #if CYD_DEBUG
    Serial.println("[SPI] Initializing SPI bus manager...");
    #endif

    s_spiMutex = xSemaphoreCreateRecursiveMutex();

    // Configure all CS pins as outputs and set HIGH (deselected)
    #if CYD_HAS_SDCARD
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    #if CYD_DEBUG
    Serial.printf("[SPI]   SD Card CS (GPIO %d) configured\n", SD_CS);
    #endif
    #endif

    #if CYD_HAS_CC1101
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
    #if CYD_DEBUG
    Serial.printf("[SPI]   CC1101 CS (GPIO %d) configured\n", CC1101_CS);
    #endif
    #endif

    #if CYD_HAS_NRF24
    pinMode(NRF24_CSN, OUTPUT);
    digitalWrite(NRF24_CSN, HIGH);
    #if CYD_DEBUG
    Serial.printf("[SPI]   NRF24 CSN (GPIO %d) configured\n", NRF24_CSN);
    #endif
    #endif

    #if CYD_HAS_PN532
    pinMode(PN532_CS, OUTPUT);
    digitalWrite(PN532_CS, HIGH);
    #if CYD_DEBUG
    Serial.printf("[SPI]   PN532 CS (GPIO %d) configured\n", PN532_CS);
    #endif
    #endif

    #if CYD_DISPLAY_ON_SHARED_SPI
    // NOTE: this only reserves the display's CS in the mutual-exclusion
    // bookkeeping below (ui_render's draw calls go through
    // spiSelectDisplay()/spiDeselect(), see ui_render.h). TFT_eSPI still
    // initializes its own driver state, but on this board/config it does
    // NOT own a separate SPI peripheral — Processors/TFT_eSPI_ESP32_S3.c
    // aliases its internal `spi` object directly to this same global SPI
    // instance (confirmed Phase 5 while wiring up CC1101 — see the update
    // in ui_render.h).
    pinMode(CYD_TFT_CS, OUTPUT);
    digitalWrite(CYD_TFT_CS, HIGH);
    #if CYD_DEBUG
    Serial.printf("[SPI]   Display CS (GPIO %d) configured (shared bus)\n", CYD_TFT_CS);
    #endif
    #endif

    // Initialize the shared SPI bus (SD/CC1101/NRF24[/PN532 on CYD]).
    // spiBus() — NOT the global SPI object — see its definition at the top of
    // this file. On T-Embed this is TFT_eSPI's HSPI instance; on CYD it is
    // the framework's global VSPI object.
    #if !defined(BOARD_M5STICK_S3)
    spiBus().begin(VSPI_SCK, VSPI_MISO, VSPI_MOSI);
    #endif

    currentDevice = SPI_DEVICE_NONE;
    s_explicitLockDepth = 0;
    initialized = true;

    #if CYD_DEBUG
    Serial.println("[SPI] SPI bus manager ready");
    Serial.printf("[SPI]   Shared bus: SCK=%d, MISO=%d, MOSI=%d\n", VSPI_SCK, VSPI_MISO, VSPI_MOSI);
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE SELECTION
// ═══════════════════════════════════════════════════════════════════════════

bool spiSelectTimeout(SPIDevice device, TickType_t ticks) {
    if (!deviceEnabled(device)) {
        #if CYD_DEBUG
        Serial.printf("[SPI] ERROR: %s not enabled in config\n", deviceName(device));
        #endif
        return false;
    }

    if (xSemaphoreTakeRecursive(s_spiMutex, ticks) != pdTRUE) {
        #if CYD_DEBUG
        Serial.printf("[SPI] TIMEOUT waiting for bus to select %s\n", deviceName(device));
        #endif
        return false;
    }

    // Deselect all first, then select the requested device.
    deselectAllCS();
    delayMicroseconds(5);   // let CS settle

    // Pre-latch this device's SPI mode/clock while EVERY CS is still deasserted,
    // so SCLK settles to the correct idle level (HIGH for the MODE3 ST7789
    // panel, LOW for the MODE0 radios/SD) BEFORE we assert this device's CS.
    //
    // Without this, switching from a MODE0 radio (NRF24/CC1101/SD) straight to
    // the MODE3 display flipped SCLK's idle level from LOW to HIGH *inside*
    // TFT_eSPI's begin_tft_write() — i.e. AFTER spiSelectDisplay() had already
    // pulled the panel's CS low. The ST7789 latched that idle-level flip as a
    // spurious clock edge, bit-shifting every following byte and wedging the
    // panel into a permanent freeze while the CPU happily kept pushing frames.
    // That was the NRF Scanner/Analyzer "screen frozen, firmware alive" hang.
    if (device != SPI_DEVICE_NONE) {
        spiBus().beginTransaction(spiGetSettings(device));
        spiBus().endTransaction();
    }

    selectCS(device);
    currentDevice = device;

    // Per-select logging is opposite of useful once anything (UI redraws,
    // radio polling) calls spiSelect() at more than a few Hz — it drowned
    // out real signal during both the Phase 2 stress test and Phase 3 UI
    // testing. Gate it behind its own flag instead of CYD_DEBUG.
    #if CYD_DEBUG && defined(SPI_MANAGER_VERBOSE)
    if (device != SPI_DEVICE_NONE) {
        Serial.print("[SPI] Selected: ");
        Serial.println(deviceName(device));
    }
    #endif

    return true;
}

bool spiSelect(SPIDevice device) {
    return spiSelectTimeout(device, portMAX_DELAY);
}

void spiDeselect() {
    deselectAllCS();
    currentDevice = SPI_DEVICE_NONE;
    xSemaphoreGiveRecursive(s_spiMutex);
}

SPIDevice spiGetSelected() {
    return currentDevice;
}

bool spiIsSelected(SPIDevice device) {
    return currentDevice == device;
}

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE-SPECIFIC HELPERS
// ═══════════════════════════════════════════════════════════════════════════

bool spiSelectSD() {
    #if CYD_HAS_SDCARD
    return spiSelect(SPI_DEVICE_SD);
    #else
    return false;
    #endif
}

bool spiSelectCC1101() {
    #if CYD_HAS_CC1101
    return spiSelect(SPI_DEVICE_CC1101);
    #else
    return false;
    #endif
}

bool spiSelectNRF24() {
    #if CYD_HAS_NRF24
    return spiSelect(SPI_DEVICE_NRF24);
    #else
    return false;
    #endif
}

bool spiSelectPN532() {
    #if CYD_HAS_PN532
    return spiSelect(SPI_DEVICE_PN532);
    #else
    return false;
    #endif
}

bool spiSelectDisplay() {
    #if CYD_DISPLAY_ON_SHARED_SPI
    return spiSelect(SPI_DEVICE_DISPLAY);
    #else
    return false;
    #endif
}

// ═══════════════════════════════════════════════════════════════════════════
// BUS LOCKING
// ═══════════════════════════════════════════════════════════════════════════

void spiLock() {
    xSemaphoreTakeRecursive(s_spiMutex, portMAX_DELAY);
    s_explicitLockDepth++;
    #if CYD_DEBUG
    Serial.println("[SPI] Bus LOCKED");
    #endif
}

void spiUnlock() {
    if (s_explicitLockDepth > 0) {
        s_explicitLockDepth--;
        xSemaphoreGiveRecursive(s_spiMutex);
    }
    #if CYD_DEBUG
    Serial.println("[SPI] Bus UNLOCKED");
    #endif
}

bool spiIsLocked() {
    return s_explicitLockDepth > 0;
}

// ═══════════════════════════════════════════════════════════════════════════
// SPI SETTINGS
// ═══════════════════════════════════════════════════════════════════════════

SPISettings spiGetSettings(SPIDevice device) {
    switch (device) {
        case SPI_DEVICE_SD:
            // SD card: 4 MHz, Mode 0, MSB first
            return SPISettings(SPI_SPEED_SD, MSBFIRST, SPI_MODE0);

        case SPI_DEVICE_CC1101:
            // CC1101: 4 MHz, Mode 0, MSB first
            return SPISettings(SPI_SPEED_CC1101, MSBFIRST, SPI_MODE0);

        case SPI_DEVICE_NRF24:
            // NRF24: 8 MHz, Mode 0, MSB first
            return SPISettings(SPI_SPEED_NRF24, MSBFIRST, SPI_MODE0);

        case SPI_DEVICE_PN532:
            // PN532: 2 MHz, Mode 0, LSB first (!) — only VSPI device using LSBFIRST
            return SPISettings(SPI_SPEED_PN532, LSBFIRST, SPI_MODE0);

        case SPI_DEVICE_DISPLAY:
            // ST7789 runs in SPI mode 3 (see TFT_eSPI's TFT_SPI_MODE default
            // for ST7789_DRIVER). This must match so the pre-latch in
            // spiSelectTimeout() settles SCLK to the panel's idle level.
            return SPISettings(SPI_SPEED_DISPLAY, MSBFIRST, SPI_MODE3);

        default:
            // Default safe settings
            return SPISettings(1000000, MSBFIRST, SPI_MODE0);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// DEBUG
// ═══════════════════════════════════════════════════════════════════════════

void spiPrintStatus() {
    #if CYD_DEBUG
    Serial.println("═══════════════════════════════════════════════════════════");
    Serial.println("              SPI BUS MANAGER STATUS");
    Serial.println("═══════════════════════════════════════════════════════════");

    Serial.print("Initialized: ");
    Serial.println(initialized ? "YES" : "NO");

    Serial.print("Bus Locked:  ");
    Serial.println(spiIsLocked() ? "YES" : "NO");

    Serial.print("Selected:    ");
    Serial.println(deviceName(currentDevice));

    Serial.println("───────────────────────────────────────────────────────────");
    Serial.println("Device Status:");

    #if CYD_HAS_SDCARD
    Serial.printf("  SD Card (GPIO %d):  %s\n", SD_CS, digitalRead(SD_CS) == LOW ? "SELECTED" : "idle");
    #else
    Serial.println("  SD Card:  DISABLED");
    #endif

    #if CYD_HAS_CC1101
    Serial.printf("  CC1101 (GPIO %d):  %s\n", CC1101_CS, digitalRead(CC1101_CS) == LOW ? "SELECTED" : "idle");
    #else
    Serial.println("  CC1101:   DISABLED");
    #endif

    #if CYD_HAS_NRF24
    Serial.printf("  NRF24 (GPIO %d):    %s\n", NRF24_CSN, digitalRead(NRF24_CSN) == LOW ? "SELECTED" : "idle");
    #else
    Serial.println("  NRF24:    DISABLED");
    #endif

    #if CYD_HAS_PN532
    Serial.printf("  PN532 (GPIO %d):   %s\n", PN532_CS, digitalRead(PN532_CS) == LOW ? "SELECTED" : "idle");
    #else
    Serial.println("  PN532:    DISABLED");
    #endif

    #if CYD_DISPLAY_ON_SHARED_SPI
    Serial.printf("  Display (GPIO %d): %s\n", CYD_TFT_CS, digitalRead(CYD_TFT_CS) == LOW ? "SELECTED" : "idle");
    #endif

    Serial.println("═══════════════════════════════════════════════════════════");
    #endif
}
