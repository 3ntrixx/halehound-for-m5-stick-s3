#ifndef SPI_MANAGER_H
#define SPI_MANAGER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound SPI Bus Manager
// CYD: manages the shared VSPI bus between SD Card, CC1101, NRF24L01, PN532.
// T-Embed CC1101 Plus: same manager also arbitrates the DISPLAY, because on
// that board the panel, CC1101, NRF24, and SD all sit on ONE physical SPI
// bus (only CS differs) — see board_t_embed_cc1101.h. CYD_DISPLAY_ON_SHARED_SPI
// (0 on CYD, 1 on T-Embed) is what turns SPI_DEVICE_DISPLAY on.
// Created: 2026-02-06. Promoted to a real FreeRTOS mutex: 2026-09-17 (T-Embed
// CC1101 port, roadmap §7 phase 2 / plan §6.3).
// ═══════════════════════════════════════════════════════════════════════════
//
// RULES:
// 1. Only ONE device can be active at a time — enforced by a real recursive
//    mutex (SemaphoreHandle_t), not a plain bool, so it is safe across the
//    core-0 (RF) / core-1 (UI/orchestration) task split (plan §3, rule 3).
// 2. Before using a device, call spiSelect(device) (or spiSelectTimeout()).
// 3. When done, call spiDeselect() to release the bus.
// 4. For a multi-step transaction that must not be interrupted, bracket it
//    with spiLock()/spiUnlock() — the mutex is recursive, so spiSelect()/
//    spiDeselect() calls nested inside a spiLock()/spiUnlock() region just
//    add/remove recursion depth without releasing the outer hold.
// 5. CS pins: LOW = selected, HIGH = deselected.
//
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <SPI.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cyd_config.h"

#ifndef CYD_DISPLAY_ON_SHARED_SPI
#define CYD_DISPLAY_ON_SHARED_SPI 0
#endif

// ═══════════════════════════════════════════════════════════════════════════
// SPI DEVICE IDENTIFIERS
// ═══════════════════════════════════════════════════════════════════════════

enum SPIDevice {
    SPI_DEVICE_NONE,      // No device selected (all CS HIGH)
    SPI_DEVICE_SD,        // SD Card
    SPI_DEVICE_CC1101,    // CC1101 SubGHz
    SPI_DEVICE_NRF24,     // NRF24L01 2.4GHz
    SPI_DEVICE_PN532,     // PN532 NFC/RFID 13.56MHz (CYD only — LSBFIRST!)
    SPI_DEVICE_DISPLAY    // T-Embed only — panel shares the radio/SD bus
};

// ═══════════════════════════════════════════════════════════════════════════
// INITIALIZATION
// ═══════════════════════════════════════════════════════════════════════════

// Initialize SPI bus, all CS pins, and the arbitration mutex.
// Call this once in setup() before using any SPI device.
void spiManagerSetup();

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE SELECTION
// ═══════════════════════════════════════════════════════════════════════════

// Select a device (deselects all others first). Blocks until the bus mutex
// is available. Returns false if the device is disabled in this board's
// config. Every successful spiSelect() must be paired with spiDeselect().
bool spiSelect(SPIDevice device);

// Same as spiSelect(), but gives up after `ticks` (pdMS_TO_TICKS(ms)) instead
// of blocking forever — for callers that must fail gracefully rather than
// stall (plan §6.3: "spiSelectTimeout so a task can fail gracefully instead
// of corrupting a transfer"). Returns false on timeout or disabled device.
bool spiSelectTimeout(SPIDevice device, TickType_t ticks);

// Deselect all devices (all CS pins HIGH) and release the bus mutex.
// Call this when done with SPI operations.
void spiDeselect();

// Get currently selected device
SPIDevice spiGetSelected();

// Check if a specific device is currently selected
bool spiIsSelected(SPIDevice device);

// ═══════════════════════════════════════════════════════════════════════════
// DEVICE-SPECIFIC HELPERS
// ═══════════════════════════════════════════════════════════════════════════

// Select SD card for file operations
// Returns true if SD card is enabled and selected
bool spiSelectSD();

// Select CC1101 for SubGHz operations
// Returns true if CC1101 is enabled and selected
bool spiSelectCC1101();

// Select NRF24 for 2.4GHz operations
// Returns true if NRF24 is enabled and selected
bool spiSelectNRF24();

// Select PN532 for NFC/RFID operations (CYD only)
// Returns true if PN532 is enabled and selected
bool spiSelectPN532();

// Select the display (T-Embed only — no-op/false on boards where the panel
// isn't on the shared bus, i.e. CYD_DISPLAY_ON_SHARED_SPI == 0).
bool spiSelectDisplay();

// ═══════════════════════════════════════════════════════════════════════════
// BUS LOCKING (for multi-step operations)
// ═══════════════════════════════════════════════════════════════════════════

// Take the bus mutex for exclusive use across multiple spiSelect/spiDeselect
// calls. Recursive: safe to call from code that may already hold it.
void spiLock();

// Release one level of spiLock(). Must be paired 1:1 with spiLock().
void spiUnlock();

// True if this task currently holds the bus mutex at any recursion depth.
bool spiIsLocked();

// ═══════════════════════════════════════════════════════════════════════════
// SHARED BUS INSTANCE
// ═══════════════════════════════════════════════════════════════════════════

// The SPIClass instance that physically owns the shared SD/CC1101/NRF24
// (and, on T-Embed, display) bus. T-Embed: TFT_eSPI's global SPIClass(HSPI)
// instance (see User_Setup_TEmbed.h) — the radio/SD code MUST use this same
// instance, not the framework's global SPI object (FSPI), or the two
// peripherals fight over GPIO 11/10/9. CYD: the framework's global SPI
// (VSPI) instance, as before.
// Callers that need transactions (e.g. the CC1101 presence probes) should
// use spiBus().beginTransaction(...)/spiBus().transfer(...).
SPIClass& spiBus();

// ═══════════════════════════════════════════════════════════════════════════
// SPI SETTINGS FOR EACH DEVICE
// ═══════════════════════════════════════════════════════════════════════════

// Get optimal SPI settings for each device
// Use with SPI.beginTransaction(spiGetSettings(device))
SPISettings spiGetSettings(SPIDevice device);

// Default SPI speeds (can be adjusted if needed)
#define SPI_SPEED_SD        4000000   // 4 MHz for SD card
#define SPI_SPEED_CC1101    4000000   // 4 MHz for CC1101
#define SPI_SPEED_NRF24     8000000   // 8 MHz for NRF24 (can handle 10MHz)
#define SPI_SPEED_PN532     2000000   // 2 MHz for PN532 (datasheet max 5MHz, conservative)
#define SPI_SPEED_DISPLAY  40000000   // 40 MHz — matches User_Setup_TEmbed.h

// ═══════════════════════════════════════════════════════════════════════════
// DEBUG
// ═══════════════════════════════════════════════════════════════════════════

// Print current SPI bus state to Serial
void spiPrintStatus();

#endif // SPI_MANAGER_H
