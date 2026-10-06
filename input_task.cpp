// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Input Task — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 3)
// ═══════════════════════════════════════════════════════════════════════════

#include "input_task.h"
#include "messaging.h"
#include "cyd_config.h"

#if CYD_HAS_ENCODER
#include "encoder_input.h"

static void inputTaskFn(void *param) {
    (void)param;
    for (;;) {
        // Runs the button/rotation-guard debounce state machines — see
        // encoder_input.cpp. Must be polled at this same ~4ms cadence the
        // debounce/rotation-guard constants were tuned against (matches
        // Momentum Firmware's own INPUT_POLL_MS on this identical board).
        encoderInputPoll();

        int delta = encoderInputGetDelta();
        while (delta > 0) {
            messagingPostInput({INPUT_EVENT_ENC_CW, millis()});
            delta--;
        }
        while (delta < 0) {
            messagingPostInput({INPUT_EVENT_ENC_CCW, millis()});
            delta++;
        }

        if (encoderInputButtonPressed()) {
            messagingPostInput({INPUT_EVENT_BTN_SHORT, millis()});
        }

        if (encoderInputButtonLongPressed()) {
            messagingPostInput({INPUT_EVENT_BTN_LONG, millis()});
        }

        if (encoderInputBackPressed()) {
            messagingPostInput({INPUT_EVENT_BACK, millis()});
        }

        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

void inputTaskStart() {
    // Core 1, priority 3 — matches the plan's task table (§4.2): tiny,
    // fast, and must outrank orchestration so encoder spam never backs up.
    xTaskCreatePinnedToCore(inputTaskFn, "input", 2048, NULL, 3, NULL, 1);
}

#endif // CYD_HAS_ENCODER

#if CYD_HAS_KEYBOARD
#include "keyboard_input.h"

// Cardputer ADV variant — keyboard_input.cpp's poll already posts
// InputEvents directly (it decodes press/release edges itself, unlike the
// encoder's raw quadrature+delta shape above), so this task is just the
// polling cadence wrapper.
static void inputTaskFnKeyboard(void *param) {
    (void)param;
    for (;;) {
        keyboardInputPoll();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTaskFnKeyboard, "input", 2048, NULL, 3, NULL, 1);
}

#endif // CYD_HAS_KEYBOARD

#if CYD_HAS_BUTTONS
#include "button_input.h"

// M5Stick S3 variant — button_input.cpp's poll already posts InputEvents
// directly (it decodes gestures itself, like keyboard_input.cpp above), so
// this task is just the polling cadence wrapper. Same cadence as the
// keyboard poll — these are plain GPIO debounce state machines, not an ISR,
// so "frequent enough that a press/release never lands between polls" is
// the only real constraint.
static void inputTaskFnButtons(void *param) {
    (void)param;
    for (;;) {
        buttonInputPoll();
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTaskFnButtons, "input", 2048, NULL, 3, NULL, 1);
}

#endif // CYD_HAS_BUTTONS
