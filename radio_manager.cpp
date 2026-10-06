// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Radio Manager — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §6.3)
// ═══════════════════════════════════════════════════════════════════════════

#include "radio_manager.h"
#include "cyd_config.h"
#include "messaging.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_radioMutex = NULL;
static RadioKind s_active = RADIO_NONE;

void radioManagerSetup() {
    s_radioMutex = xSemaphoreCreateMutex();
    s_active = RADIO_NONE;
}

bool radioAcquire(RadioKind kind) {
    if (!s_radioMutex) return false;

    bool ok = false;
    xSemaphoreTake(s_radioMutex, portMAX_DELAY);
    if (s_active == RADIO_NONE || s_active == kind) {
        // Workers publish before releasing the radio. Clear their remaining
        // messages before a new worker can publish into the shared mailbox.
        if (s_active == RADIO_NONE && telemetry_q) {
            xQueueReset(telemetry_q);
        }
        s_active = kind;
        ok = true;
    }
    xSemaphoreGive(s_radioMutex);

    #if CYD_DEBUG
    if (!ok) {
        Serial.printf("[RADIO] ERROR: radio %d already active, cannot acquire %d\n", (int)s_active, (int)kind);
    }
    #endif
    return ok;
}

void radioRelease(RadioKind kind) {
    if (!s_radioMutex) return;

    xSemaphoreTake(s_radioMutex, portMAX_DELAY);
    if (s_active == kind) {
        s_active = RADIO_NONE;
    }
    xSemaphoreGive(s_radioMutex);
}

RadioKind radioGetActive() {
    return s_active;
}
