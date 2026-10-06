#ifndef PSRAM_POOL_H
#define PSRAM_POOL_H

// ═══════════════════════════════════════════════════════════════════════════
// HaleHound PSRAM Fixed-Block Pool Allocator
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §5.2)
//
// Backs capture_q / log_q (messaging.h): producers on core 0 acquire a
// block, fill it, enqueue the handle; the consumer releases it after
// render/write. No malloc/free in the producer path — that's the plan's
// stated reason this matters ("the single most important change for
// keeping RX from stalling", §5.2) since heap contention/fragmentation
// across cores is exactly what stalls a hot RX path.
//
// Never touch this from an ISR or promiscuous-mode callback (cache-miss
// stalls on the PSRAM path, plan §8 risk table).
//
// BOARD_HAS_PSRAM selects where the pool actually lives: PSRAM on boards
// that have it (ps_malloc), or a small internal-SRAM block on boards that
// don't (Cardputer ADV — 512KB SRAM total, no PSRAM die at all; see
// board_cardputer_adv.h and the port plan §6). Same API either way, so
// storage_manager.cpp (the only consumer) needs no board branch of its own.
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>

#if BOARD_HAS_PSRAM
#define PSRAM_POOL_BLOCK_SIZE   4096
#define PSRAM_POOL_BLOCK_COUNT    32
#else
// SRAM budget note (port plan §6): conservative until confirmed on
// hardware alongside the WiFi/BLE stacks + the 64.8KB display sprite —
// 8 blocks x 1KB = 8KB total, enough for eapol_sniff's PCAP/.hc22000
// writes (which chunk well under 1KB per storageCaptureBytes() call) and
// wardrive-style CSV log lines, without meaningfully denting the 512KB
// budget. Revisit both constants after checking ESP.getFreeHeap() with a
// feature actually capturing.
#define PSRAM_POOL_BLOCK_SIZE   1024
#define PSRAM_POOL_BLOCK_COUNT     8
#endif

// Allocates PSRAM_POOL_BLOCK_COUNT * PSRAM_POOL_BLOCK_SIZE and builds the
// free list. Call once from setup(). Returns false if allocation fails
// (caller should treat that as fatal for capture features).
bool psramPoolInit();

// Acquires a free block. Returns a handle >= 0, or -1 if the pool is
// exhausted (caller should drop the capture/log and count it — plan §5.3).
int32_t psramPoolAcquire();

// Returns a block to the pool. Safe to call from a different task than the
// one that acquired it (that's the whole point — producer fills, consumer
// releases after render/write).
void psramPoolRelease(int32_t handle);

// Pointer to a block's data. Valid until psramPoolRelease(handle).
uint8_t* psramPoolData(int32_t handle);

constexpr size_t psramPoolBlockSize() { return PSRAM_POOL_BLOCK_SIZE; }

// Blocks currently checked out — surface this on a status line (§5.3).
uint32_t psramPoolUsedCount();

#endif // PSRAM_POOL_H
