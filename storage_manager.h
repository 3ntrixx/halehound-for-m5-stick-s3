#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Storage Manager — Phase 6 (roadmap §7 phase 6 / plan §4.2's
// `storage` task: "Serializes SD/NVS writes so RF tasks never block on
// flash. Consumes LogMsg.")
// Created: 2026-09-17 (T-Embed CC1101 port)
//
// Owns queued SD writes and capture resets. Producers never
// call SD.* directly — they call storageLogLine(), which acquires a
// PSRAM pool block (psram_pool.h), copies the line in, and posts a
// LogMsg on log_q (messaging.h). This task drains log_q, appends the
// block's bytes to the right file, and releases the block back to the
// pool. If the card is behind (or missing), messagingPostLog() drops +
// counts (droppedLogMsgs) rather than blocking the caller — a stalled SD
// card must never stall a radio task (plan §4.2's stated reason for this
// task existing at all, and the new CC1101/SD-share-a-bus risk in plan
// §7's risk table).
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <FS.h>

enum StorageFileId {
    STORAGE_FILE_WARDRIVE_LOG,
    STORAGE_FILE_COUNT,
};

// Mounts the SD card and starts the storage task. Call once from setup(),
// after spiManagerSetup(). Returns false if no card is present/mountable
// — storageLogLine() then just drops everything (droppedLogMsgs), so
// callers don't need their own "do we have a card" branch.
bool storageManagerSetup();

bool storageIsCardPresent();

// Lock and access the mounted filesystem for bounded readers (Loot and
// Saved Captures). On T-Embed this arbitrates SD's SPI bus; on StickS3 it
// serializes LittleFS access against the storage writer task. Pair every
// successful storageAcquire() with storageRelease().
bool storageAcquire();
void storageRelease();
fs::FS &storageFilesystem();

// Convenience for the common "log one NUL-terminated text line" producer
// pattern (plan §5.2's acquire/fill/enqueue shape). Normalizes trailing CR/LF
// to one newline, truncating content to psramPoolBlockSize() - 1 if needed.
// Success means queued, not persisted; write failures increment droppedLogMsgs.
// Returns false if the
// card is absent or a block/queue slot isn't available right now — the
// caller should treat that the same as messagingPostLog() (drop and move
// on, never retry-block a radio/UI thread on storage).
bool storageLogLine(uint8_t fileId, const char *line);

// ─────────────────────────────────────────────────────────────────────────
// capture_q support (messaging.h) — first real consumer, added for
// eapol_sniff.cpp. Reset commands and byte writes share the same FIFO.
// Only the storage task changes the destination, after older writes finish.
// Producers serialize queue submissions; reset waits for acknowledgement.
// One logical capture producer owns the current target. Multiple simultaneous
// capture sessions would require an explicit per-session destination API.
// ─────────────────────────────────────────────────────────────────────────

// Deletes any existing file at `path` (a fresh capture session must not
// silently append onto stale data behind a format-defining header, e.g.
// a PCAP global header) and makes it the target for storageCaptureBytes().
// Waits for previous queued writes and the reset to finish. Returns false
// for invalid/overlong paths or failed removal; never silently truncates paths.
// Call once when a capture feature starts, from a normal task only.
bool storageResetCaptureFile(const char *path);

// Acquires a PSRAM block, copies `len` raw bytes in, and posts a
// CaptureMsg on capture_q. Safe to call from a normal task context only —
// NEVER from an ISR or promiscuous-mode RX callback (psram_pool.h: PSRAM
// access there risks a cache-miss stall). A callback that needs to feed
// this must stage the frame in a small SRAM buffer first and hand it off
// from a task, the way eapol_sniff.cpp does. Oversize records are rejected
// rather than silently truncated. Success means queued, not persisted.
bool storageCaptureBytes(const uint8_t *data, size_t len);

// Creates `path` as a directory if it doesn't already exist. Call before
// writing into a subdirectory for the first time (SD.open() with
// FILE_APPEND/FILE_WRITE does not create missing parent directories).
bool storageEnsureDir(const char *path);

// One-off direct write to an arbitrary, caller-named path — for
// infrequent outputs whose filename isn't known ahead of time (e.g. a
// capture's SSID-derived .hc22000 file) and so doesn't fit the fixed
// StorageFileId table. Appends `text` (NUL-terminated). Does the SD I/O
// synchronously on the CALLING task (arbitrated by spi_manager like
// everything else here) — call only from a normal task context, and only
// for rare/one-shot writes, never a hot path (use storageLogLine()/
// storageCaptureBytes() for anything frequent). Returns false on open failure
// or a short write; true means File.write accepted all requested bytes.
bool storageAppendTextFile(const char *path, const char *text);

// Opens a fresh file and holds the storage lock until storageCloseFile().
// Intended for bounded, one-shot structured exports such as a GATT dump.
bool storageOpenOverwriteFile(const char *path, File *file);
void storageCloseFile(File *file);

#endif // STORAGE_MANAGER_H
