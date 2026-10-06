#ifndef LOOT_VIEWER_TEMBED_H
#define LOOT_VIEWER_TEMBED_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Loot Viewer — T-Embed CC1101 port
// Created: 2026-10-03
//
// SD card browser for the per-feature loot folders that other T-Embed
// features write into:
//     /wardriving/         wardrive_lite.cpp CSVs
//     /eapol/              eapol_sniff.cpp .hc22000 captures
//     /wp_loot/            whisper_pair_tembed.cpp attack reports
//     /race_loot/          airoha_race_tembed.cpp intel reports
//     /loot/               generic BLE loot (hp/fy from bluetooth_attacks)
//
// A simplified port of loot_manager.cpp's hub-and-subviewer design — the
// CYD build splits into LootManager + SavedCaptures + WPLootViewer with
// per-category wardriving-stats parsing, delete flow and color-coded text.
// This T-Embed port keeps the structure but drops those: it's a plain
// three-phase browser (hub → file list → file viewer), encoder-driven,
// view-only.  Delete + stats are deferred — they can be grafted onto the
// same phase machine later without reshaping the orchestrator wiring.
//
// Structurally follows subghz_subread.cpp: a background scan/load worker
// (SD reads are brief but still bracketed by spi_manager, so running them
// off-orchestrator keeps input handling responsive) that posts
// RENDER_MSG_SCAN_COMPLETE on each phase transition.  Nothing here is a
// "running" background feature — the worker scans, parks, and exits on
// STOP, same shape as deauther / eapol / subread's SCAN phase.
// ═══════════════════════════════════════════════════════════════════════════

enum LootViewerPhase {
    LV_PHASE_IDLE,
    LV_PHASE_HUB,         // category list ready
    LV_PHASE_SCANNING,    // scanning a category dir
    LV_PHASE_FILE_LIST,   // files in selected category ready
    LV_PHASE_LOADING,     // reading a file into the view buffer
    LV_PHASE_FILE_VIEW,   // file content ready to scroll
};

void lootViewerStart();
void lootViewerStop();
LootViewerPhase lootViewerGetPhase();

// Hub (always valid once LV_PHASE_HUB is reached — populated on start).
int lootViewerGetHubCount();
const char *const *lootViewerGetHubLines();

// File list (valid once LV_PHASE_FILE_LIST is reached).
int lootViewerGetFileListCount();
bool lootViewerHasFiles();
const char *const *lootViewerGetFileListLines();

// File view (valid once LV_PHASE_FILE_VIEW is reached).
int lootViewerGetFileViewCount();
const char *const *lootViewerGetFileViewLines();

// Title to show in the submenu header for whatever phase is current.
const char *lootViewerGetTitle();

// Commands (orchestrator → worker via Cmd queue).
void lootViewerSelectCategory(int index);  // HUB -> SCANNING -> FILE_LIST
void lootViewerSelectFile(int index);      // FILE_LIST -> LOADING -> FILE_VIEW
void lootViewerBack();                     // FILE_VIEW -> FILE_LIST, FILE_LIST -> HUB

#endif // LOOT_VIEWER_TEMBED_H
