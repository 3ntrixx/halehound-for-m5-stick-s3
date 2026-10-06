#ifndef BATTERY_GAUGE_H
#define BATTERY_GAUGE_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound T-Embed — BQ27220 Fuel Gauge
// Created: 2026-09-18 (T-Embed CC1101 port, UI parity pass)
//
// Reads the on-board BQ27220 over i2c_manager (BOARD_I2C_SDA/SCL). Command
// map/read protocol match LilyGo's own bq27xxx_test example: StateOfCharge
// 0x2C, Voltage 0x08 (mV), BatteryStatus 0x0A (bit0 DSG: 0 = charging).
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>

// Probes the gauge once (call after i2cManagerSetup()).
void batteryGaugeSetup();

// Spawns the background task that polls the gauge every few seconds. Runs
// on its own core/task specifically so a wedged I2C transaction (bus glitch,
// stuck slave) can't block the orchestrator's loop() — and with it, all
// input handling and screen updates — the way a direct loop()-driven poll
// once did (see git history for the NRF Scanner/Analyzer freeze this fixed).
void batteryGaugeStart();

int batteryGaugePercent();     // 0-100, or -1 when unavailable
int batteryGaugeMillivolts();  // mV, or -1 when unavailable
bool batteryGaugeCharging();
bool batteryGaugeAvailable();

#endif // BATTERY_GAUGE_H
