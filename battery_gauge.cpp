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

// M5Stick S3's M5PM1 power-management IC (board_m5stick_s3.h's
// BOARD_I2C_SDA/SCL bus) has no dedicated fuel-gauge/state-of-charge
// register like the T-Embed's BQ27220 above — only a raw battery-voltage
// ADC (readVbat()), a power-source enum (getPowerSource(): one of
// 5VIN/5VINOUT/BAT/UNKNOWN) and a charge-enable config bit. There's no
// charging-in-progress/charge-complete status bit anywhere in the public
// API (checked M5PM1.h's m5pm1_pwr_cfg_t and m5pm1_irq_sys_t — only
// 5VIN/5VINOUT/BAT insert-remove IRQ flags exist), so batteryGaugeCharging()
// below is "external power present AND charge circuit enabled", the
// closest available proxy — it can still read charging after the pack is
// actually topped off. Percent comes from interpolating VBAT against a
// single-cell LiPo open-circuit-voltage curve (mvToPercent()/kLipoCurve
// below) rather than a naive linear 3300-4200mV map, since LiPo voltage
// sags non-linearly (flat through the 3.7-3.9V midrange, steep at both
// ends) — still an approximation under load (no coulomb counter to correct
// for IR drop while radios are transmitting), so treat it as "close" above
// a resting battery rather than lab-accurate. Shares the single M5PM1
// instance boardPowerSetup() already brought up (see board_power_m5stick_s3.h)
// instead of re-running begin(), which would tear down and rebuild the I2C
// bus a second time.

#include "board_power_m5stick_s3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <M5PM1.h>

#define BATTERY_READ_INTERVAL_MS  5000

static volatile int s_percent = -1;
static volatile int s_millivolts = -1;
static volatile bool s_charging = false;
static volatile bool s_available = false;

// Single-cell LiPo open-circuit-voltage -> state-of-charge breakpoints,
// highest voltage first (a commonly-used approximation of the 3.7V LiPo
// discharge curve — not pulled from this specific cell's datasheet, so
// still worth checking against hardware). mvToPercent() linearly
// interpolates between adjacent pairs and clamps past both ends.
struct MvPct { uint16_t mv; uint8_t pct; };
static const MvPct kLipoCurve[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80},
    {3980, 75},  {3950, 70}, {3910, 65}, {3870, 60}, {3850, 55},
    {3840, 50},  {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30},
    {3750, 25},  {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},
    {3270, 0},
};
#define LIPO_CURVE_LEN (sizeof(kLipoCurve) / sizeof(kLipoCurve[0]))

static int mvToPercent(int mv) {
    if (mv >= kLipoCurve[0].mv) return 100;
    if (mv <= kLipoCurve[LIPO_CURVE_LEN - 1].mv) return 0;
    for (unsigned i = 0; i < LIPO_CURVE_LEN - 1; i++) {
        uint16_t mvHi = kLipoCurve[i].mv, mvLo = kLipoCurve[i + 1].mv;
        if (mv <= mvHi && mv >= mvLo) {
            int pctHi = kLipoCurve[i].pct, pctLo = kLipoCurve[i + 1].pct;
            return pctLo + (int)((long)(mv - mvLo) * (pctHi - pctLo) / (mvHi - mvLo));
        }
    }
    return 0; // unreachable — table is monotonic and covers [curve-min, curve-max]
}

static bool batteryGaugeRead() {
    M5PM1 *pm1 = boardPowerPm1();
    if (!pm1) return false;

    // Average a few consecutive reads — the PMIC ADC's noise isn't
    // documented, and averaging is cheap insurance against single-sample
    // jitter (same reasoning as the Cardputer ADV ADC-divider path above).
    const int kSamples = 4;
    uint32_t sum = 0;
    int ok = 0;
    for (int i = 0; i < kSamples; i++) {
        uint16_t mv = 0;
        if (pm1->readVbat(&mv) == M5PM1_OK && mv >= 2500 && mv <= 4400) {
            sum += mv;
            ok++;
        }
        if (i < kSamples - 1) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (ok == 0) return false;

    int mv = (int)(sum / (uint32_t)ok);
    s_millivolts = mv;
    s_percent = mvToPercent(mv);

    m5pm1_pwr_src_t src = M5PM1_PWR_SRC_UNKNOWN;
    uint8_t cfg = 0;
    bool extPower = (pm1->getPowerSource(&src) == M5PM1_OK) &&
                    (src == M5PM1_PWR_SRC_5VIN || src == M5PM1_PWR_SRC_5VINOUT);
    bool chargeEnabled = (pm1->getPowerConfig(&cfg) == M5PM1_OK) &&
                         (cfg & M5PM1_PWR_CFG_CHG_EN);
    s_charging = extPower && chargeEnabled;
    return true;
}

void batteryGaugeSetup() {
    s_available = batteryGaugeRead();
    #if CYD_DEBUG
    if (s_available) {
        Serial.printf("[BATTERY] M5PM1 VBAT: %d mV (%d%%)\n", s_millivolts, s_percent);
    } else {
        Serial.println("[BATTERY] M5PM1 unavailable (boardPowerSetup() didn't bring up the PMIC, or VBAT read failed)");
    }
    #endif
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
    xTaskCreatePinnedToCore(batteryGaugeTask, "battery_pm1", 2048, NULL, 1, NULL, 0);
}

int batteryGaugePercent() { return s_available ? s_percent : -1; }
int batteryGaugeMillivolts() { return s_available ? s_millivolts : -1; }
bool batteryGaugeCharging() { return s_charging; }
bool batteryGaugeAvailable() { return s_available; }

#else // !(CYD_HAS_FUELGAUGE && CYD_HAS_I2C_BUS) && !BOARD_CARDPUTER_ADV && !BOARD_M5STICK_S3

void batteryGaugeSetup() {}
void batteryGaugeStart() {}
int batteryGaugePercent() { return -1; }
int batteryGaugeMillivolts() { return -1; }
bool batteryGaugeCharging() { return false; }
bool batteryGaugeAvailable() { return false; }

#endif
