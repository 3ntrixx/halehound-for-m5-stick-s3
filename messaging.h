#ifndef MESSAGING_H
#define MESSAGING_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Cross-Core Messaging Contract
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §5.1)
//
// POD message types passed BY VALUE through FreeRTOS queues — never a
// pointer into a producer's stack. Replaces the CYD build's ad-hoc
// volatile *Ready/*Done flags and polled result arrays (plan §5.1
// migration mapping). Queues themselves are created by messagingInit();
// producers/consumers are wired up module-by-module starting Phase 3.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ─────────────────────────────────────────────────────────────────────────
// ui_input_q — input task -> orchestrator. Depth 16, drop-oldest on full
// (encoder spam is fine to coalesce).
// ─────────────────────────────────────────────────────────────────────────
enum InputEventType {
    INPUT_EVENT_ENC_CW,
    INPUT_EVENT_ENC_CCW,
    INPUT_EVENT_BTN_SHORT,
    INPUT_EVENT_BTN_LONG,
    INPUT_EVENT_BACK,
};

struct InputEvent {
    InputEventType type;
    uint32_t timestampMs;
};

// ─────────────────────────────────────────────────────────────────────────
// telemetry_q — radio/dsp -> ui_render. Depth 8, mailbox semantics (newest
// wins) unless a message is explicitly a terminal/one-shot kind.
// ─────────────────────────────────────────────────────────────────────────
enum RenderMsgKind {
    RENDER_MSG_STATUS_TEXT,     // short status line update
    RENDER_MSG_SCAN_COMPLETE,   // terminal — replaces *ScanDone booleans
    RENDER_MSG_SPECTRUM_FRAME,  // replaces jam_detect's *FrameReady flags
    RENDER_MSG_WIFI_MONITOR,    // WiFi packet monitor: live counters + 13 channels
    RENDER_MSG_DROP_COUNTER,    // backpressure/overrun counter update (§5.3)
};

#define RENDER_MSG_TEXT_MAX      48
// 96 bins: SubGHz's 33-entry sweep and the NRF24 scanner's 85 channels
// (2400-2484MHz, nrf_scanner.h) both fit.
#define RENDER_MSG_SPECTRUM_BINS 128

#define WIFI_MONITOR_BINS 96

struct WifiMonitorMsg {
    uint32_t totalPackets;
    uint32_t deauthPackets;
    uint16_t packetsThisFrame;
    uint8_t channel;
    uint8_t spectrum[WIFI_MONITOR_BINS];
};

static_assert(sizeof(WifiMonitorMsg) <= RENDER_MSG_SPECTRUM_BINS,
              "WiFi monitor telemetry must fit in RenderMsg payload");

struct RenderMsg {
    RenderMsgKind kind;
    union {
        char text[RENDER_MSG_TEXT_MAX];
        uint8_t spectrum[RENDER_MSG_SPECTRUM_BINS];
        WifiMonitorMsg wifiMonitor;
        uint32_t dropCount;
    } payload;
};

// ─────────────────────────────────────────────────────────────────────────
// capture_q — capture_rx -> storage + ui_render. Depth 32. Payload is a
// HANDLE into the PSRAM fixed-block pool (psram_pool.h), never a copy.
// Block briefly, then drop + count (never stall RX — see droppedCaptureMsgs).
// ─────────────────────────────────────────────────────────────────────────
#define CAPTURE_PATH_MAX 64
enum CaptureMsgKind { CAPTURE_WRITE, CAPTURE_RESET };

struct CaptureMsg {
    CaptureMsgKind kind;
    int32_t poolHandle;   // psram_pool.h handle; -1 = invalid
    size_t len;           // bytes valid in the block
    char path[CAPTURE_PATH_MAX]; // RESET only; copied, never a caller's pointer
};

// ─────────────────────────────────────────────────────────────────────────
// cmd_q — orchestrator -> worker. One per active worker/feature, depth 4,
// blocking enqueue (commands are rare and must not be lost). Created per
// feature via messagingCreateCmdQueue(), not a single global.
// ─────────────────────────────────────────────────────────────────────────
enum CmdType {
    CMD_STOP,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_SET_CHANNEL,
    CMD_SET_FREQ,
};

struct Cmd {
    CmdType type;
    int32_t intArg;
    float floatArg;
};

// ─────────────────────────────────────────────────────────────────────────
// log_q — any -> storage. Depth 16. Payload is a PSRAM pool handle, like
// capture_q. Drop + count if SD is behind; never stall RF for logging.
// ─────────────────────────────────────────────────────────────────────────
struct LogMsg {
    uint8_t fileId;
    int32_t poolHandle;
    size_t len;
};

// ═══════════════════════════════════════════════════════════════════════════
// GLOBAL QUEUE HANDLES
// ═══════════════════════════════════════════════════════════════════════════

extern QueueHandle_t ui_input_q;
extern QueueHandle_t telemetry_q;
extern QueueHandle_t capture_q;
extern QueueHandle_t log_q;

// Drop/overrun counters (plan §5.3 — "every stream queue carries a visible
// dropped/overrun counter"). Read these from the module's status line.
extern volatile uint32_t droppedInputEvents;
extern volatile uint32_t droppedCaptureMsgs;
extern volatile uint32_t droppedLogMsgs;

// Creates ui_input_q / telemetry_q / capture_q / log_q. Call once from
// setup() before starting any task that touches them.
void messagingInit();

// Per-feature command queue — caller owns the handle's lifetime and must
// call vQueueDelete() when the feature's worker(s) are torn down.
QueueHandle_t messagingCreateCmdQueue();

// ═══════════════════════════════════════════════════════════════════════════
// ENQUEUE HELPERS — centralize each queue's overflow policy (plan §5.1
// table) so callers don't reimplement it ad hoc.
// ═══════════════════════════════════════════════════════════════════════════

// Drop-oldest on full: pops the oldest event to make room, then pushes.
// Never blocks — safe to call from an ISR-adjacent input task.
void messagingPostInput(const InputEvent &ev);

// Mailbox semantics: overwrites the queue's single pending item so readers
// always see the newest telemetry. Never blocks.
void messagingPostTelemetry(const RenderMsg &msg);

// Blocks briefly (a few ticks), then drops + increments droppedCaptureMsgs
// on failure. Never stalls the RX path waiting indefinitely.
bool messagingPostCapture(const CaptureMsg &msg);

// Blocks briefly, then drops + increments droppedLogMsgs on failure.
bool messagingPostLog(const LogMsg &msg);

#endif // MESSAGING_H
