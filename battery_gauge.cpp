// ═══════════════════════════════════════════════════════════════════════════
// HaleHound T-Embed — BQ27220 Fuel Gauge (implementation)
// Created: 2026-09-18 (T-Embed CC1101 port, UI parity pass)
// ═══════════════════════════════════════════════════════════════════════════

#include "battery_gauge.h"
#include "cyd_config.h"

#if CYD_HAS_FUELGAUGE && CYD_HAS_I2C_BUS

#include <Wire.h>
#include "i2c_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BQ27220_ADDR              0x55
#define BQ27220_CMD_VOLTAGE       0x08
#define BQ27220_CMD_BATTERY_STATUS 0x0A
#define BQ27220_CMD_SOC           0x2C

#define BATTERY_READ_INTERVAL_MS  5000

static volatile int s_percent = -1;
static volatile int s_millivolts = -1;
static volatile bool s_charging = false;
static volatile bool s_available = false;

// Vendor-example read protocol: command byte with a stop, then a 2-byte
// read, LSB first.
static bool readWord(uint8_t cmd, uint16_t *out) {
    uint8_t lo = 0, hi = 0;

    i2cLock();
    Wire.beginTransmission(BQ27220_ADDR);
    Wire.write(cmd);
    if (Wire.endTransmission(true) != 0) {
        i2cUnlock();
        return false;
    }
    if (Wire.requestFrom((uint8_t)BQ27220_ADDR, (uint8_t)2) != 2) {
        i2cUnlock();
        return false;
    }
    lo = (uint8_t)Wire.read();
    hi = (uint8_t)Wire.read();
    i2cUnlock();

    *out = (uint16_t)(lo | (hi << 8));
    return true;
}

void batteryGaugeSetup() {
    uint16_t mv = 0;
    if (readWord(BQ27220_CMD_VOLTAGE, &mv) && mv >= 2000 && mv <= 5000) {
        s_available = true;
        s_millivolts = (int)mv;
        #if CYD_DEBUG
        Serial.printf("[BATTERY] BQ27220 detected: %u mV\n", mv);
        #endif
    } else {
        #if CYD_DEBUG
        Serial.println("[BATTERY] BQ27220 not detected");
        #endif
    }
}

static void batteryGaugeRead() {
    uint16_t mv = 0, soc = 0, status = 0;
    if (readWord(BQ27220_CMD_VOLTAGE, &mv) && mv >= 2000 && mv <= 5000) {
        s_millivolts = (int)mv;
    }
    if (readWord(BQ27220_CMD_SOC, &soc) && soc <= 100) {
        s_percent = (int)soc;
    }
    if (readWord(BQ27220_CMD_BATTERY_STATUS, &status)) {
        // BatteryStatus bit0 = DSG; clear while charging.
        s_charging = ((status & 0x0001) == 0);
    }
}

static void batteryGaugeTask(void *param) {
    (void)param;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BATTERY_READ_INTERVAL_MS));
        batteryGaugeRead();
    }
}

void batteryGaugeStart() {
    if (!s_available) return;
    xTaskCreatePinnedToCore(batteryGaugeTask, "battery_gauge", 2048, NULL, 1, NULL, 0);
}

int batteryGaugePercent() { return s_percent; }
int batteryGaugeMillivolts() { return s_millivolts; }
bool batteryGaugeCharging() { return s_charging; }
bool batteryGaugeAvailable() { return s_available; }

#elif defined(BOARD_CARDPUTER_ADV)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Cardputer ADV: plain ADC divider (BATTERY_ADC_PIN, ratio BATTERY_ADC_RATIO
// — board_cardputer_adv.h), no PMIC/fuel-gauge I2C part. Exposes the same
// batteryGauge*() API the T-Embed's BQ27220 path does above so
// board_bringup_cardputer.cpp's Device Info screen / loop() battery-change
// redraw need no board branch of their own. No charging-state signal is
// available from a plain divider — batteryGaugeCharging() always false.

#define BATTERY_MV_FULL            4200
#define BATTERY_MV_EMPTY           3300
#define BATTERY_READ_INTERVAL_MS   3000

static volatile int s_percent = -1;
static volatile int s_millivolts = -1;

static void batteryAdcRead() {
    // Averaged over a few samples — a single ADC1 read is noisy enough on
    // the S3 to visibly jitter the displayed percentage between polls.
    uint32_t sum = 0;
    const int kSamples = 8;
    for (int i = 0; i < kSamples; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
    }
    int mv = (int)((sum / kSamples) * BATTERY_ADC_RATIO);
    s_millivolts = mv;

    int pct = (int)(((mv - BATTERY_MV_EMPTY) * 100L) / (BATTERY_MV_FULL - BATTERY_MV_EMPTY));
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_percent = pct;
}

void batteryGaugeSetup() {
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    batteryAdcRead();
    #if CYD_DEBUG
    Serial.printf("[BATTERY] ADC divider: %d mV (%d%%)\n", s_millivolts, s_percent);
    #endif
}

static void batteryGaugeTask(void *param) {
    (void)param;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BATTERY_READ_INTERVAL_MS));
        batteryAdcRead();
    }
}

void batteryGaugeStart() {
    xTaskCreatePinnedToCore(batteryGaugeTask, "battery_adc", 2048, NULL, 1, NULL, 0);
}

int batteryGaugePercent() { return s_percent; }
int batteryGaugeMillivolts() { return s_millivolts; }
bool batteryGaugeCharging() { return false; }
bool batteryGaugeAvailable() { return true; }

#elif defined(BOARD_M5STICK_S3)

// M5Stick S3's M5PM1 power-management IC is I2C (board_m5stick_s3.h's
// BOARD_I2C_SDA/SCL bus), but its register map isn't verified against real
// hardware in this pass — inventing addresses here would just produce a
// confidently wrong battery percentage on-device instead of an honest
// "unavailable". Wire this up for real once hardware is in hand (outline
// plan §2's bring-up milestone: "Validate any available battery readings
// rather than copying Cardputer's ADC implementation or T-Embed's
// fuel-gauge driver" — i.e. this needs its own verified driver, not a
// guess). Device Info / About screens already handle percent==-1 as "n/a".

void batteryGaugeSetup() {
    #if CYD_DEBUG
    Serial.println("[BATTERY] M5PM1 driver not yet implemented — reporting unavailable");
    #endif
}
void batteryGaugeStart() {}
int batteryGaugePercent() { return -1; }
int batteryGaugeMillivolts() { return -1; }
bool batteryGaugeCharging() { return false; }
bool batteryGaugeAvailable() { return false; }

#else // !(CYD_HAS_FUELGAUGE && CYD_HAS_I2C_BUS) && !BOARD_CARDPUTER_ADV && !BOARD_M5STICK_S3

void batteryGaugeSetup() {}
void batteryGaugeStart() {}
int batteryGaugePercent() { return -1; }
int batteryGaugeMillivolts() { return -1; }
bool batteryGaugeCharging() { return false; }
bool batteryGaugeAvailable() { return false; }

#endif
