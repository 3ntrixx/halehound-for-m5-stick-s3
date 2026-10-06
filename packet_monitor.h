#ifndef PACKET_MONITOR_H
#define PACKET_MONITOR_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Packet Monitor — Phase 4 reference port
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 4)
//
// Second reference lifecycle, deliberately different from wifi_scan.cpp's
// one-shot scan: promiscuous-mode packet counting plus the original CYD
// 256-sample FFT, streamed to the render task for the mirrored area graph
// and waterfall. Encoder rotation replaces the original CH-/CH+ touch icons.
// ═══════════════════════════════════════════════════════════════════════════

void packetMonitorStart();
void packetMonitorAdjustChannel(int delta);

// Call from the orchestrator when BACK is pressed. Blocks until the
// worker has actually exited (radio released).
void packetMonitorStop();

#endif // PACKET_MONITOR_H
