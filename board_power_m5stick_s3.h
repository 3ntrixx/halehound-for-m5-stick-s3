#ifndef BOARD_POWER_M5STICK_S3_H
#define BOARD_POWER_M5STICK_S3_H

// Enables the M5PM1 L3B rail that supplies the StickS3 LCD backlight.
// Call after i2cManagerSetup() and before uiRenderStart().
bool boardPowerSetup();

#endif
