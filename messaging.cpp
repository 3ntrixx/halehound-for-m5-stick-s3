// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Cross-Core Messaging — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §5.1)
// ═══════════════════════════════════════════════════════════════════════════

#include "messaging.h"

QueueHandle_t ui_input_q = NULL;
QueueHandle_t telemetry_q = NULL;
QueueHandle_t capture_q = NULL;
QueueHandle_t log_q = NULL;

volatile uint32_t droppedInputEvents = 0;
volatile uint32_t droppedCaptureMsgs = 0;
volatile uint32_t droppedLogMsgs = 0;

#define UI_INPUT_Q_DEPTH    16
#define TELEMETRY_Q_DEPTH    8
#define CAPTURE_Q_DEPTH     32
#define LOG_Q_DEPTH         16
#define CMD_Q_DEPTH          4

void messagingInit() {
    ui_input_q  = xQueueCreate(UI_INPUT_Q_DEPTH, sizeof(InputEvent));
    telemetry_q = xQueueCreate(TELEMETRY_Q_DEPTH, sizeof(RenderMsg));
    capture_q   = xQueueCreate(CAPTURE_Q_DEPTH, sizeof(CaptureMsg));
    log_q       = xQueueCreate(LOG_Q_DEPTH, sizeof(LogMsg));
}

QueueHandle_t messagingCreateCmdQueue() {
    return xQueueCreate(CMD_Q_DEPTH, sizeof(Cmd));
}

void messagingPostInput(const InputEvent &ev) {
    if (xQueueSend(ui_input_q, &ev, 0) != pdTRUE) {
        // Full — drop the oldest to make room for the newest, matching the
        // "encoder spam is fine to coalesce" policy (plan §5.1).
        InputEvent discard;
        xQueueReceive(ui_input_q, &discard, 0);
        droppedInputEvents++;
        xQueueSend(ui_input_q, &ev, 0);
    }
}

void messagingPostTelemetry(const RenderMsg &msg) {
    // Mailbox: always keep only the newest message queued.
    RenderMsg discard;
    xQueueReceive(telemetry_q, &discard, 0);
    xQueueSend(telemetry_q, &msg, 0);
}

bool messagingPostCapture(const CaptureMsg &msg) {
    if (xQueueSend(capture_q, &msg, pdMS_TO_TICKS(5)) == pdTRUE) {
        return true;
    }
    droppedCaptureMsgs++;
    return false;
}

bool messagingPostLog(const LogMsg &msg) {
    if (xQueueSend(log_q, &msg, pdMS_TO_TICKS(5)) == pdTRUE) {
        return true;
    }
    droppedLogMsgs++;
    return false;
}
