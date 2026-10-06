#ifndef KARMA_ATTACK_TEMBED_H
#define KARMA_ATTACK_TEMBED_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Karma Attack — T-Embed CC1101 port
// Created: 2026-10-03
//
// Port of karma_attack.cpp's Phase-1 (COLLECT) probe-request listener into
// the T-Embed worker-task + ui_render architecture.  Phase-2 (ATTACK) is
// still done by captive_portal.cpp as it is on CYD, but the handoff is now
// orchestrator-driven: the user selects an SSID from Karma's live list and
// the orchestrator pre-sets captive_portal's SSID (captivePortalSetSSID())
// before calling captivePortalReadyEnter(), matching the CYD flow's
// "stop collection → CaptivePortal::setSSID() → setup() → startPortal()".
//
// Structurally close to probe_sniffer.cpp (same promiscuous RX callback
// shape, same SPSC ring, same channel-hopping worker on core 0, same
// SCAN_COMPLETE live-refresh), but deduplicated by SSID instead of by
// client MAC so the user sees one row per probed network — same shape as
// the CYD original's ssidPool.
//
// Not ported: the DEBUG counters (dbgCallbackHits), the per-row recent-
// probe blink highlighting (the submenu renderer is one color), and the
// three separate status fields on the collection screen (probes / ssids /
// clients) — collapsed into a single status-text line through the normal
// RENDER_MSG_STATUS_TEXT path, like every other T-Embed feature.
// ═══════════════════════════════════════════════════════════════════════════

void karmaStart();
void karmaStop();

// Result accessors — stay valid for as long as the feature is running,
// not just after one scan (same contract as probe_sniffer.h).
const char *const *karmaGetResultLines();
int karmaGetResultCount();

// Raw SSID string at the given index — used by the orchestrator to seed
// captive_portal's SSID buffer before handing off to the evil-twin flow.
// Returns NULL when the index is out of range.
const char *karmaGetSsid(int index);

// Snapshot of counters for the running status line: "P:1234 S:12 C:7".
void karmaFormatStatus(char *buf, int bufLen);

#endif // KARMA_ATTACK_TEMBED_H
