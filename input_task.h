#ifndef INPUT_TASK_H
#define INPUT_TASK_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Input Task
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 3 / plan §4.2/§6.2)
//
// Wraps encoder_input.cpp's ISR-fed decode/debounce into the dedicated,
// high-priority "input" task from the plan's task table: polls the driver
// and posts typed InputEvent structs onto ui_input_q (messaging.h) instead
// of callers polling encoder_input directly (that was Phase 1/2 scaffolding
// — see board_bringup_t_embed.cpp's earlier revisions).
//
// Mapping: encoder rotate CW/CCW -> DOWN/UP (arbitrary convention, flip in
// input_task.cpp if it feels backwards), encoder push -> SELECT (BTN_SHORT),
// BOARD_USER_KEY -> BACK.
// ═══════════════════════════════════════════════════════════════════════════

// Starts the input task (core 1, high priority, per plan §4.2 task table).
// Call once from setup(), after messagingInit() and encoderInputSetup().
void inputTaskStart();

#endif // INPUT_TASK_H
