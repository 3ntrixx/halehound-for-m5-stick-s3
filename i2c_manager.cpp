// ═══════════════════════════════════════════════════════════════════════════
// HaleHound I2C Bus Manager — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §6.3)
// ═══════════════════════════════════════════════════════════════════════════

#include "i2c_manager.h"

#if CYD_HAS_I2C_BUS

#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_i2cMutex = NULL;

void i2cManagerSetup() {
    s_i2cMutex = xSemaphoreCreateRecursiveMutex();
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);

    #if CYD_DEBUG
    Serial.printf("[I2C] Bus ready: SDA=%d, SCL=%d\n", BOARD_I2C_SDA, BOARD_I2C_SCL);
    #endif
}

void i2cLock() {
    xSemaphoreTakeRecursive(s_i2cMutex, portMAX_DELAY);
}

void i2cUnlock() {
    xSemaphoreGiveRecursive(s_i2cMutex);
}

#endif // CYD_HAS_I2C_BUS
