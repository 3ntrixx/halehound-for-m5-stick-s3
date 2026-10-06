// ═══════════════════════════════════════════════════════════════════════════
// HaleHound M5Stack StickS3 — Two-Button Gesture Driver (implementation)
// Created: 2026-10-04 (StickS3 port, Stage 3 — buttons and compact menus)
// ═══════════════════════════════════════════════════════════════════════════

#include "button_input.h"

#if CYD_HAS_BUTTONS

#include "messaging.h"

// Debounce shape matches encoder_input.cpp's agree-polls state machine
// (same board family). Polled, not ISR-driven: a press/release only counts
// once N consecutive polls agree.
#define BTN_DEBOUNCE_POLLS   2U
#define BTN_LONG_PRESS_MS  500U

struct ButtonState {
    uint8_t pin;
    bool rawPressed;
    bool debouncedPressed;
    uint8_t agreePolls;
    uint32_t pressStartedMs;
    bool longSent;       // this press cycle already fired its own long-press event
    bool chordConsumed;  // this press cycle already fired the A+B chord event
};

static ButtonState s_btnA;
static ButtonState s_btnB;

static void buttonStateInit(ButtonState *btn, uint8_t pin) {
    btn->pin = pin;
    btn->rawPressed = (digitalRead(pin) == LOW); // active-low, INPUT_PULLUP
    btn->debouncedPressed = btn->rawPressed;
    btn->agreePolls = BTN_DEBOUNCE_POLLS;
    btn->pressStartedMs = millis();
    btn->longSent = false;
    btn->chordConsumed = false;
}

// A: short press -> Next, long press -> Select. Suppressed entirely (no
// short OR long emitted) once an A+B chord has claimed this press cycle.
static void pollButtonA(uint32_t now) {
    bool raw = (digitalRead(s_btnA.pin) == LOW);
    if (raw == s_btnA.rawPressed) {
        if (s_btnA.agreePolls < BTN_DEBOUNCE_POLLS) s_btnA.agreePolls++;
    } else {
        s_btnA.rawPressed = raw;
        s_btnA.agreePolls = 1;
    }
    if (s_btnA.agreePolls < BTN_DEBOUNCE_POLLS) return;

    if (s_btnA.debouncedPressed == s_btnA.rawPressed) {
        // Held steady — check for the long-press threshold.
        if (s_btnA.debouncedPressed && !s_btnA.longSent && !s_btnA.chordConsumed &&
            (now - s_btnA.pressStartedMs) >= BTN_LONG_PRESS_MS) {
            s_btnA.longSent = true;
            messagingPostInput({INPUT_EVENT_BTN_SHORT, now}); // A long = Select
        }
        return;
    }

    // Debounced state changed.
    s_btnA.debouncedPressed = s_btnA.rawPressed;
    if (s_btnA.debouncedPressed) {
        s_btnA.pressStartedMs = now;
        s_btnA.longSent = false;
        s_btnA.chordConsumed = false;
    } else {
        // Release — short press only if neither a long-press nor a chord
        // already fired for this press cycle.
        if (!s_btnA.longSent && !s_btnA.chordConsumed) {
            messagingPostInput({INPUT_EVENT_ENC_CW, now}); // A short = Next
        }
    }
}

// B: short press -> Previous, long press -> Back/stop. Same suppression
// rule as A above.
static void pollButtonB(uint32_t now) {
    bool raw = (digitalRead(s_btnB.pin) == LOW);
    if (raw == s_btnB.rawPressed) {
        if (s_btnB.agreePolls < BTN_DEBOUNCE_POLLS) s_btnB.agreePolls++;
    } else {
        s_btnB.rawPressed = raw;
        s_btnB.agreePolls = 1;
    }
    if (s_btnB.agreePolls < BTN_DEBOUNCE_POLLS) return;

    if (s_btnB.debouncedPressed == s_btnB.rawPressed) {
        if (s_btnB.debouncedPressed && !s_btnB.longSent && !s_btnB.chordConsumed &&
            (now - s_btnB.pressStartedMs) >= BTN_LONG_PRESS_MS) {
            s_btnB.longSent = true;
            messagingPostInput({INPUT_EVENT_BACK, now}); // B long = Back/stop
        }
        return;
    }

    s_btnB.debouncedPressed = s_btnB.rawPressed;
    if (s_btnB.debouncedPressed) {
        s_btnB.pressStartedMs = now;
        s_btnB.longSent = false;
        s_btnB.chordConsumed = false;
    } else {
        if (!s_btnB.longSent && !s_btnB.chordConsumed) {
            messagingPostInput({INPUT_EVENT_ENC_CCW, now}); // B short = Previous
        }
    }
}

void buttonInputSetup() {
    pinMode(BOARD_BTN_A_PIN, INPUT_PULLUP);
    pinMode(BOARD_BTN_B_PIN, INPUT_PULLUP);
    buttonStateInit(&s_btnA, BOARD_BTN_A_PIN);
    buttonStateInit(&s_btnB, BOARD_BTN_B_PIN);
}

void buttonInputPoll() {
    uint32_t now = millis();
    pollButtonA(now);
    pollButtonB(now);

    // A+B chord -> secondary action. Checked every poll (not just once at
    // the long-press threshold) so it latches within one debounce window of
    // both buttons going down together — well before BTN_LONG_PRESS_MS can
    // elapse — which is what lets pollButtonA/B's own long-press checks
    // above see chordConsumed==true in time and stay silent. Gated on
    // !longSent too: a chord is only "both pressed together", not "B joins
    // in after A already fired its own long-press action".
    if (s_btnA.debouncedPressed && s_btnB.debouncedPressed &&
        !s_btnA.chordConsumed && !s_btnB.chordConsumed &&
        !s_btnA.longSent && !s_btnB.longSent) {
        s_btnA.chordConsumed = true;
        s_btnB.chordConsumed = true;
        messagingPostInput({INPUT_EVENT_BTN_LONG, now}); // A+B = secondary action
    }
}

#endif // CYD_HAS_BUTTONS
