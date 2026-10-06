#ifndef BEACON_SPAMMER_H
#define BEACON_SPAMMER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Beacon Spammer — reference port
// Created: 2026-09-18 (T-Embed CC1101 port, roadmap §7 fidelity pass)
//
// A port of wifi_attacks.cpp's BeaconSpammer namespace's TX core: the same
// 128-byte 802.11 beacon frame template, the same 40-entry joke SSID list
// for "Normal" mode (fixed channel, one random SSID per packet), and the
// same "Nuke" mode (random 6-char SSID + random channel every packet) —
// both byte offsets identical to the original. Deliberate efficiency
// deviations from the original (see beaconTxTask() in the .cpp):
//   - channel is only retuned when it actually changes (the original
//     retuned every packet, draining the raw-TX queue);
//   - TX return codes are checked and the task yields on a full queue
//     instead of blind-sending every frame 3x;
//   - random SA/BSSID bytes are forced to valid unicast addresses, so
//     scanners don't discard ~half the frames as multicast.
// Starts spamming immediately on entry (this project's established
// convention for every continuous feature — see packet_monitor.cpp etc. —
// rather than the original's separate touch "Start" arm step).
//
// Encoder rotate adjusts the channel in Normal mode (wraps 1-14, same as
// the original's CH-/CH+ touch icons); press toggles Normal<->Nuke mode in
// place, replacing the original's separate blocking "tap Nuke, tap to
// exit" sub-screen with the encoder-toggle idiom already used elsewhere in
// this project (EAPOL's deauth toggle). Not ported: the Matrix rain visual
// and the mushroom-cloud Nuke watermark — this project's uiRenderShowStatus()
// is a single status line, like every other *_RUNNING screen here.
// ═══════════════════════════════════════════════════════════════════════════

void beaconSpammerStart();
void beaconSpammerStop();

// Encoder rotate while running: delta is +1 (CW) or -1 (CCW), wraps 1-14.
// No-op in Nuke mode (channel is randomized per-packet there, matching the
// original — CH-/CH+ only affected bsChannel, which Nuke mode never reads).
void beaconSpammerAdjustChannel(int delta);

// Encoder press while running: toggles Normal <-> Nuke mode.
void beaconSpammerToggleNuke();

#endif // BEACON_SPAMMER_H
