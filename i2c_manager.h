#ifndef I2C_MANAGER_H
#define I2C_MANAGER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound I2C Bus Manager
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §6.3)
//
// Single owner for the BQ27220 fuel gauge / BQ25896 charger (and optional
// PN532, if the NFC add-on is fitted — see CYD_HAS_PN532 in
// board_t_embed_cc1101.h) on BOARD_I2C_SDA/SCL. Wire's TwoWire is not safe
// to drive concurrently from two tasks, so battery_monitor + any future I2C
// user must bracket their transactions with i2cLock()/i2cUnlock().
//
// Only compiled when the board actually has an I2C peripheral bus
// (CYD_HAS_I2C_BUS — set in board_t_embed_cc1101.h; CYD has none today).
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "cyd_config.h"

#if CYD_HAS_I2C_BUS

// Calls Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL). Call once from setup().
void i2cManagerSetup();

// Take/release exclusive access to the bus for a transaction. Recursive —
// safe to call from code that may already hold it.
void i2cLock();
void i2cUnlock();

#endif // CYD_HAS_I2C_BUS

#endif // I2C_MANAGER_H
