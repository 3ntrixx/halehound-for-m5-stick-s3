#include "board_power_m5stick_s3.h"

#include <Arduino.h>
#include <M5PM1.h>
#include <Wire.h>
#include "board_m5stick_s3.h"

static M5PM1 s_pm1;

bool boardPowerSetup() {
    // M5Unified normally enables this rail. This port owns its display with
    // TFT_eSPI, so initialize just the PMIC instead of a second display stack.
    if (s_pm1.begin(&Wire, M5PM1_DEFAULT_ADDR, BOARD_I2C_SDA,
                    BOARD_I2C_SCL, M5PM1_I2C_FREQ_100K) != M5PM1_OK)
        return false;

    const m5pm1_gpio_num_t rail = M5PM1_GPIO_NUM_2;
    bool ok = s_pm1.gpioSetFunc(rail, M5PM1_GPIO_FUNC_GPIO) == M5PM1_OK;
    ok = (s_pm1.gpioSetMode(rail, M5PM1_GPIO_MODE_OUTPUT) == M5PM1_OK) && ok;
    ok = (s_pm1.gpioSetDrive(rail, M5PM1_GPIO_DRIVE_PUSHPULL) == M5PM1_OK) && ok;
    ok = (s_pm1.gpioSetOutput(rail, true) == M5PM1_OK) && ok;
    if (ok) Serial.println("[POWER] M5PM1 L3B rail enabled");
    return ok;
}
