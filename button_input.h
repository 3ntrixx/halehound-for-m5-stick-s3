#ifndef BUTTON_INPUT_H
#define BUTTON_INPUT_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound M5Stack StickS3 — Two-Button Gesture Driver
// Created: 2026-10-04 (StickS3 port, Stage 3 — buttons and compact menus;
// see PORT_M5StickS3_WiFi-BLE_Outline.md §3)
//
// This board has exactly two application buttons (A on BOARD_BTN_A_PIN, B on
// BOARD_BTN_B_PIN — board_m5stick_s3.h) and no rotary encoder, so the usual
// five-event InputEvent model (messaging.h) has to come out of button
// GESTURES instead of quadrature + a push:
//
//   A short press  -> INPUT_EVENT_ENC_CW    (Next)
//   B short press  -> INPUT_EVENT_ENC_CCW   (Previous)
//   A long press   -> INPUT_EVENT_BTN_SHORT (Select)
//   B long press   -> INPUT_EVENT_BACK      (Back / stop)
//   A+B chord      -> INPUT_EVENT_BTN_LONG  (Secondary action — filters,
//                                            rescans, feature-specific
//                                            controls)
//
// Like keyboard_input.cpp (the Cardputer ADV equivalent of this file),
// buttonInputPoll() decodes raw presses straight into InputEvents and posts
// them onto ui_input_q itself — everything downstream of messaging.h's
// five-event model (ui_render.*, board_bringup_m5stick_s3.cpp) is unchanged.
//
// Debounce/long-press shape follows encoder_input.cpp's agree-polls state
// machine (same board family, same "poll at a few ms, don't trust a single
// raw read" reasoning), not a from-scratch design.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "cyd_config.h"

#if CYD_HAS_BUTTONS

// Configures BOARD_BTN_A_PIN/BOARD_BTN_B_PIN as inputs and seeds the
// debounce state machines. Call once from setup().
void buttonInputSetup();

// Polls both buttons, runs the debounce/long-press/chord state machines,
// and posts any resulting InputEvent onto ui_input_q (messaging.h). Call
// repeatedly from the input task at a few ms cadence (input_task.cpp).
void buttonInputPoll();

#endif // CYD_HAS_BUTTONS

#endif // BUTTON_INPUT_H
