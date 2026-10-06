// ═══════════════════════════════════════════════════════════════════════════
// HaleHound PSRAM Fixed-Block Pool Allocator — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 2 / plan §5.2)
// ═══════════════════════════════════════════════════════════════════════════

#include "psram_pool.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static uint8_t *s_pool = NULL;
static bool s_freeList[PSRAM_POOL_BLOCK_COUNT];
static SemaphoreHandle_t s_poolMutex = NULL;
static uint32_t s_usedCount = 0;

bool psramPoolInit() {
#if BOARD_HAS_PSRAM
    s_pool = (uint8_t *)ps_malloc((size_t)PSRAM_POOL_BLOCK_SIZE * PSRAM_POOL_BLOCK_COUNT);
#else
    // No PSRAM on this board (e.g. Cardputer ADV's StampS3A) — small
    // internal-SRAM block instead, see the reduced constants in
    // psram_pool.h. Plain malloc(), not heap_caps_malloc(): this pool's
    // whole point is producers never allocate on the hot path, so which
    // heap capability serves it doesn't matter, only that it's not PSRAM.
    s_pool = (uint8_t *)malloc((size_t)PSRAM_POOL_BLOCK_SIZE * PSRAM_POOL_BLOCK_COUNT);
#endif
    if (!s_pool) {
        #if CYD_DEBUG
        Serial.println("[PSRAM_POOL] ERROR: pool allocation failed");
        #endif
        return false;
    }

    for (int i = 0; i < PSRAM_POOL_BLOCK_COUNT; i++) {
        s_freeList[i] = true;
    }
    s_usedCount = 0;
    s_poolMutex = xSemaphoreCreateMutex();

    #if CYD_DEBUG
    Serial.printf("[PSRAM_POOL] %d blocks x %d bytes ready\n", PSRAM_POOL_BLOCK_COUNT, PSRAM_POOL_BLOCK_SIZE);
    #endif
    return true;
}

int32_t psramPoolAcquire() {
    if (!s_poolMutex) return -1;

    int32_t handle = -1;
    xSemaphoreTake(s_poolMutex, portMAX_DELAY);
    for (int i = 0; i < PSRAM_POOL_BLOCK_COUNT; i++) {
        if (s_freeList[i]) {
            s_freeList[i] = false;
            s_usedCount++;
            handle = i;
            break;
        }
    }
    xSemaphoreGive(s_poolMutex);
    return handle;
}

void psramPoolRelease(int32_t handle) {
    if (!s_poolMutex || handle < 0 || handle >= PSRAM_POOL_BLOCK_COUNT) return;

    xSemaphoreTake(s_poolMutex, portMAX_DELAY);
    if (!s_freeList[handle]) {
        s_freeList[handle] = true;
        s_usedCount--;
    }
    xSemaphoreGive(s_poolMutex);
}

uint8_t* psramPoolData(int32_t handle) {
    if (!s_pool || handle < 0 || handle >= PSRAM_POOL_BLOCK_COUNT) return NULL;
    return s_pool + ((size_t)handle * PSRAM_POOL_BLOCK_SIZE);
}

uint32_t psramPoolUsedCount() {
    return s_usedCount;
}
