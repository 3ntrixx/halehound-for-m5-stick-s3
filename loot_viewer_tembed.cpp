// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Loot Viewer — T-Embed CC1101 implementation
// Created: 2026-10-03  (see loot_viewer_tembed.h for the port writeup)
// ═══════════════════════════════════════════════════════════════════════════

#include "loot_viewer_tembed.h"
#include <Arduino.h>
#include "cyd_config.h"
#include "messaging.h"
#include "storage_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ═══════════════════════════════════════════════════════════════════════════
// LAYOUT CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════

#ifdef BOARD_M5STICK_S3
#define LV_HUB_COUNT       3
#else
#define LV_HUB_COUNT       7
#endif
#define LV_HUB_LINE_LEN    40

#define LV_MAX_FILES       32
#define LV_FILE_LINE_LEN   40

#define LV_MAX_VIEW_LINES  256
#define LV_VIEW_LINE_LEN   42

#define LV_MAX_PATH        96

// Cmd queue reuses — same convention as the Bluetooth-port files.
#define LV_CMD_SCAN_DIR    CMD_RESUME        // intArg = category index
#define LV_CMD_LOAD_FILE   CMD_PAUSE         // intArg = file index

// ═══════════════════════════════════════════════════════════════════════════
// CATEGORIES
// ═══════════════════════════════════════════════════════════════════════════

struct Category {
    const char *label;    // user-visible name
    const char *dir;      // SD path (no trailing slash)
};

static const Category s_cats[LV_HUB_COUNT] = {
    {"Wardriving",  "/wardriving"},
    {"EAPOL",       "/eapol"},
#ifndef BOARD_M5STICK_S3
    {"WhisperPair", "/wp_loot"},
    {"Airoha RACE", "/race_loot"},
    {"BLE Loot",    "/loot"},
#endif
    {"GATT Dumps",  "/gatt"},
#ifndef BOARD_M5STICK_S3
    {"RFID Dumps",  "/rfid"},
#endif
};

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t      s_workerHandle = NULL;
static QueueHandle_t     s_cmdQueue     = NULL;
static SemaphoreHandle_t s_doneSem      = NULL;

static volatile LootViewerPhase s_phase = LV_PHASE_IDLE;

static int s_catIndex   = -1;   // which category is open (-1 at HUB)
static int s_fileIndex  = -1;   // which file is open    (-1 at FILE_LIST)

// Hub lines — rebuilt once on start (file counts per category).
static char  s_hubBuf[LV_HUB_COUNT][LV_HUB_LINE_LEN];
static const char *s_hubPtrs[LV_HUB_COUNT];

// File list — populated on category select; survives back-nav to HUB,
// so a repeat select without a re-scan still shows the same files.
static char  s_fileBuf[LV_MAX_FILES][LV_FILE_LINE_LEN];
static const char *s_filePtrs[LV_MAX_FILES];
static char  s_fileNames[LV_MAX_FILES][LV_FILE_LINE_LEN];  // raw basenames
static int   s_fileCount = 0;

// File view — scrollable text buffer.
static char  s_viewBuf[LV_MAX_VIEW_LINES][LV_VIEW_LINE_LEN];
static const char *s_viewPtrs[LV_MAX_VIEW_LINES];
static int   s_viewCount = 0;

static char  s_currentTitle[32] = "Loot";

// ═══════════════════════════════════════════════════════════════════════════
// TELEMETRY HELPERS
// ═══════════════════════════════════════════════════════════════════════════

static void postScanComplete() {
    RenderMsg msg;
    msg.kind = RENDER_MSG_SCAN_COMPLETE;
    messagingPostTelemetry(msg);
}

static void postStatus(const char *text) {
    RenderMsg msg;
    msg.kind = RENDER_MSG_STATUS_TEXT;
    strncpy(msg.payload.text, text, RENDER_MSG_TEXT_MAX - 1);
    msg.payload.text[RENDER_MSG_TEXT_MAX - 1] = '\0';
    messagingPostTelemetry(msg);
}

// ═══════════════════════════════════════════════════════════════════════════
// HUB — count files per category
// ═══════════════════════════════════════════════════════════════════════════

static int countFilesIn(const char *dir) {
    if (!storageFilesystem().exists(dir)) return 0;
    File d = storageFilesystem().open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        return 0;
    }
    int count = 0;
    File entry;
    while ((entry = d.openNextFile())) {
        if (!entry.isDirectory()) count++;
        entry.close();
    }
    d.close();
    return count;
}

static void buildHub() {
    bool haveStorage = storageAcquire();
    for (int i = 0; i < LV_HUB_COUNT; i++) {
        int n = haveStorage ? countFilesIn(s_cats[i].dir) : 0;
        if (haveStorage) {
            snprintf(s_hubBuf[i], LV_HUB_LINE_LEN, "%s (%d)", s_cats[i].label, n);
        } else {
            snprintf(s_hubBuf[i], LV_HUB_LINE_LEN, "%s (offline)", s_cats[i].label);
        }
        s_hubPtrs[i] = s_hubBuf[i];
    }
    if (haveStorage) storageRelease();
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE LIST — scan selected category directory
// ═══════════════════════════════════════════════════════════════════════════

static void scanCategoryDir(int catIdx) {
    s_fileCount = 0;
    if (catIdx < 0 || catIdx >= LV_HUB_COUNT) return;
    const char *dir = s_cats[catIdx].dir;

    if (!storageAcquire()) return;

    if (!storageFilesystem().exists(dir)) {
        storageRelease();
        return;
    }

    File d = storageFilesystem().open(dir);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        storageRelease();
        return;
    }

    File entry;
    while ((entry = d.openNextFile()) && s_fileCount < LV_MAX_FILES) {
        if (entry.isDirectory()) { entry.close(); continue; }

        const char *name = entry.name();
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;

        strncpy(s_fileNames[s_fileCount], base, LV_FILE_LINE_LEN - 1);
        s_fileNames[s_fileCount][LV_FILE_LINE_LEN - 1] = '\0';
        s_fileCount++;
        entry.close();
    }
    d.close();
    storageRelease();
}

static void formatFileLines() {
    for (int i = 0; i < s_fileCount; i++) {
        // Truncate .txt / .csv extensions in the display line for density.
        char nm[LV_FILE_LINE_LEN];
        strncpy(nm, s_fileNames[i], sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = '\0';
        int l = strlen(nm);
        if (l > 4 && (strcasecmp(nm + l - 4, ".txt") == 0 ||
                      strcasecmp(nm + l - 4, ".csv") == 0 ||
                      strcasecmp(nm + l - 4, ".sub") == 0)) {
            nm[l - 4] = '\0';
        }
        snprintf(s_fileBuf[i], LV_FILE_LINE_LEN, "%s", nm);
        s_filePtrs[i] = s_fileBuf[i];
    }
    if (s_fileCount == 0) {
        snprintf(s_fileBuf[0], LV_FILE_LINE_LEN,
                 storageIsCardPresent() ? "(empty)" : "(storage offline)");
        s_filePtrs[0] = s_fileBuf[0];
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE VIEW — read the file and wrap into fixed-width lines
// ═══════════════════════════════════════════════════════════════════════════

static void appendViewLine(const char *src, int len) {
    if (s_viewCount >= LV_MAX_VIEW_LINES) return;
    if (len > LV_VIEW_LINE_LEN - 1) len = LV_VIEW_LINE_LEN - 1;
    memcpy(s_viewBuf[s_viewCount], src, len);
    s_viewBuf[s_viewCount][len] = '\0';
    s_viewPtrs[s_viewCount] = s_viewBuf[s_viewCount];
    s_viewCount++;
}

static void loadFile(int catIdx, int fileIdx) {
    s_viewCount = 0;
    if (catIdx < 0 || catIdx >= LV_HUB_COUNT ||
        fileIdx < 0 || fileIdx >= s_fileCount) return;

    if (!storageAcquire()) return;

    char path[LV_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s",
             s_cats[catIdx].dir, s_fileNames[fileIdx]);

    File f = storageFilesystem().open(path, FILE_READ);
    if (!f) {
        storageRelease();
        appendViewLine("<open failed>", 13);
        return;
    }

    // Wrap long lines at LV_VIEW_LINE_LEN - 1; break on '\n'; swallow '\r'.
    char line[LV_VIEW_LINE_LEN];
    int li = 0;
    while (f.available() && s_viewCount < LV_MAX_VIEW_LINES) {
        char c = (char)f.read();
        if (c == '\r') continue;
        if (c == '\n') {
            appendViewLine(line, li);
            li = 0;
            continue;
        }
        if (li >= LV_VIEW_LINE_LEN - 1) {
            appendViewLine(line, li);
            li = 0;
            if (s_viewCount >= LV_MAX_VIEW_LINES) break;
        }
        line[li++] = c;
    }
    if (li > 0 && s_viewCount < LV_MAX_VIEW_LINES) {
        appendViewLine(line, li);
    }

    f.close();
    storageRelease();

    if (s_viewCount == 0) {
        appendViewLine("<empty file>", 12);
    } else if (s_viewCount >= LV_MAX_VIEW_LINES) {
        // Replace the tail line with a truncation marker so it's visible.
        const char *trunc = "... [truncated]";
        strncpy(s_viewBuf[LV_MAX_VIEW_LINES - 1], trunc, LV_VIEW_LINE_LEN - 1);
        s_viewBuf[LV_MAX_VIEW_LINES - 1][LV_VIEW_LINE_LEN - 1] = '\0';
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER TASK
// ═══════════════════════════════════════════════════════════════════════════

static void workerTask(void *param) {
    (void)param;

    // Initial hub population — synchronous SD read, posts SCAN_COMPLETE
    // so the orchestrator can flip IDLE -> HUB without a race.
    buildHub();
    s_phase = LV_PHASE_HUB;
    strncpy(s_currentTitle, "Loot", sizeof(s_currentTitle) - 1);
    postScanComplete();

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        if (cmd.type == CMD_STOP) {
            break;
        } else if (cmd.type == LV_CMD_SCAN_DIR) {
            s_catIndex = (int)cmd.intArg;
            postStatus("Scanning...");
            scanCategoryDir(s_catIndex);
            formatFileLines();
            if (s_catIndex >= 0 && s_catIndex < LV_HUB_COUNT) {
                strncpy(s_currentTitle, s_cats[s_catIndex].label,
                        sizeof(s_currentTitle) - 1);
                s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
            }
            s_phase = LV_PHASE_FILE_LIST;
            postScanComplete();
        } else if (cmd.type == LV_CMD_LOAD_FILE) {
            s_fileIndex = (int)cmd.intArg;
            postStatus("Loading...");
            loadFile(s_catIndex, s_fileIndex);
            if (s_fileIndex >= 0 && s_fileIndex < s_fileCount) {
                // Title: trimmed basename of the file.
                char tmp[sizeof(s_currentTitle)];
                strncpy(tmp, s_fileNames[s_fileIndex], sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                int l = strlen(tmp);
                if (l > 4 && (strcasecmp(tmp + l - 4, ".txt") == 0 ||
                              strcasecmp(tmp + l - 4, ".csv") == 0)) {
                    tmp[l - 4] = '\0';
                }
                strncpy(s_currentTitle, tmp, sizeof(s_currentTitle) - 1);
                s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
            }
            s_phase = LV_PHASE_FILE_VIEW;
            postScanComplete();
        }
    }

    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void lootViewerStart() {
    if (s_cmdQueue) return;    // already started

    s_phase       = LV_PHASE_SCANNING;    // transient — worker flips to HUB
    s_catIndex    = -1;
    s_fileIndex   = -1;
    s_fileCount   = 0;
    s_viewCount   = 0;

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem  = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(workerTask, "loot_viewer", 6144,
                            NULL, 2, &s_workerHandle, 0);
}

void lootViewerStop() {
    if (!s_cmdQueue) return;

    Cmd cmd;
    cmd.type = CMD_STOP;
    xQueueSend(s_cmdQueue, &cmd, portMAX_DELAY);
    xSemaphoreTake(s_doneSem, portMAX_DELAY);

    vQueueDelete(s_cmdQueue);
    vSemaphoreDelete(s_doneSem);
    s_cmdQueue     = NULL;
    s_doneSem      = NULL;
    s_workerHandle = NULL;
    s_phase        = LV_PHASE_IDLE;
}

LootViewerPhase lootViewerGetPhase() {
    return s_phase;
}

int lootViewerGetHubCount() { return LV_HUB_COUNT; }
const char *const *lootViewerGetHubLines() { return s_hubPtrs; }

int lootViewerGetFileListCount() {
    return (s_fileCount > 0) ? s_fileCount : 1;    // "(empty)" fallback row
}
bool lootViewerHasFiles() { return s_fileCount > 0; }
const char *const *lootViewerGetFileListLines() { return s_filePtrs; }

int lootViewerGetFileViewCount() {
    return (s_viewCount > 0) ? s_viewCount : 1;
}
const char *const *lootViewerGetFileViewLines() { return s_viewPtrs; }

const char *lootViewerGetTitle() { return s_currentTitle; }

void lootViewerSelectCategory(int index) {
    if (!s_cmdQueue || index < 0 || index >= LV_HUB_COUNT) return;
    s_phase = LV_PHASE_SCANNING;
    Cmd cmd;
    cmd.type   = LV_CMD_SCAN_DIR;
    cmd.intArg = index;
    xQueueSend(s_cmdQueue, &cmd, 0);
}

void lootViewerSelectFile(int index) {
    if (!s_cmdQueue || s_fileCount <= 0 ||
        index < 0 || index >= s_fileCount) return;
    s_phase = LV_PHASE_LOADING;
    Cmd cmd;
    cmd.type   = LV_CMD_LOAD_FILE;
    cmd.intArg = index;
    xQueueSend(s_cmdQueue, &cmd, 0);
}

void lootViewerBack() {
    // Pure orchestrator-side state transition — the lower level's data is
    // still cached, so stepping back is instant without a re-scan.
    if (s_phase == LV_PHASE_FILE_VIEW) {
        s_viewCount = 0;
        s_phase = LV_PHASE_FILE_LIST;
        if (s_catIndex >= 0 && s_catIndex < LV_HUB_COUNT) {
            strncpy(s_currentTitle, s_cats[s_catIndex].label,
                    sizeof(s_currentTitle) - 1);
            s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
        }
    } else if (s_phase == LV_PHASE_FILE_LIST) {
        s_fileCount = 0;
        s_catIndex  = -1;
        s_phase = LV_PHASE_HUB;
        strncpy(s_currentTitle, "Loot", sizeof(s_currentTitle) - 1);
        s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
    }
}
