#ifndef SAVED_CAPTURES_TEMBED_H
#define SAVED_CAPTURES_TEMBED_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Saved Captures — T-Embed CC1101 port
// Created: 2026-10-03
//
// A simplified port of saved_captures.cpp's /eapol browser.  The CYD build
// is a stand-alone screen with its own touch UI, a per-file detail view
// that parses .hc22000 hash lines + PCAP magic, and a delete-with-
// confirmation flow.  This port keeps the browse-and-view core (which is
// what the row in SIGINT_SUBMENU actually exposes) and defers delete and
// the deeper PCAP parse until there's a user ask for them — grafted onto
// the same two-phase worker without reshaping the orchestrator wiring.
//
// Structurally mirrors loot_viewer_tembed.cpp pinned to /eapol: a worker
// task on core 0 scans the dir, loads a selected file into a scroll buffer,
// and posts RENDER_MSG_SCAN_COMPLETE on each phase transition.  Row text
// shows the trimmed basename + size suffix (e.g. "wpa_capture (3 KB)") so
// a glance tells apart a stub from a real handshake.
// ═══════════════════════════════════════════════════════════════════════════

enum SavedCapturesPhase {
    SC_PHASE_IDLE,
    SC_PHASE_SCANNING,    // scanning /eapol
    SC_PHASE_FILE_LIST,   // files ready
    SC_PHASE_LOADING,     // reading a file into the view buffer
    SC_PHASE_FILE_VIEW,   // file content ready to scroll
};

void savedCapturesStart();
void savedCapturesStop();
SavedCapturesPhase savedCapturesGetPhase();

// File list (valid once SC_PHASE_FILE_LIST is reached).
int  savedCapturesGetFileListCount();
bool savedCapturesHasFiles();
const char *const *savedCapturesGetFileListLines();

// File view (valid once SC_PHASE_FILE_VIEW is reached).
int  savedCapturesGetFileViewCount();
const char *const *savedCapturesGetFileViewLines();

// Title to show in the submenu header for whatever phase is current.
const char *savedCapturesGetTitle();

// Commands (orchestrator → worker via Cmd queue).
void savedCapturesSelectFile(int index); // FILE_LIST -> LOADING -> FILE_VIEW
void savedCapturesBack();                // FILE_VIEW -> FILE_LIST

#endif // SAVED_CAPTURES_TEMBED_H
