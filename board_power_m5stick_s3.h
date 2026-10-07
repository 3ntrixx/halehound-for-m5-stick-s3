#ifndef BOARD_POWER_M5STICK_S3_H
#define BOARD_POWER_M5STICK_S3_H

// Enables the M5PM1 L3B rail that supplies the StickS3 LCD backlight.
// Call after i2cManagerSetup() and before uiRenderStart().
bool boardPowerSetup();

// The M5PM1 instance boardPowerSetup() brought up, or nullptr if
// boardPowerSetup() hasn't run yet or its begin() failed. battery_gauge.cpp
// reads VBAT/power-source through this shared handle instead of owning a
// second M5PM1 and re-running begin() — M5PM1::begin() tears down and
// rebuilds the I2C bus (Wire.end()/Wire.begin()) on every call, which a
// second instance would otherwise trigger on top of the one boardPowerSetup()
// already did.
class M5PM1;
M5PM1 *boardPowerPm1();

#endif
