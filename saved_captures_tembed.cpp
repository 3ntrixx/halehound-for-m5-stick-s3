// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Saved Captures — T-Embed CC1101 implementation
// Created: 2026-10-03  (see saved_captures_tembed.h for the port writeup)
// ═══════════════════════════════════════════════════════════════════════════

#include "saved_captures_tembed.h"
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

#define SC_DIR             "/eapol"

#define SC_MAX_FILES       32
#define SC_FILE_LINE_LEN   40

#define SC_MAX_VIEW_LINES  128
#define SC_VIEW_LINE_LEN   42

#define SC_MAX_PATH        96

// Cmd queue reuses — same convention as loot_viewer_tembed.cpp.
#define SC_CMD_SCAN_DIR    CMD_RESUME
#define SC_CMD_LOAD_FILE   CMD_PAUSE         // intArg = file index

// ═══════════════════════════════════════════════════════════════════════════
// MODULE STATE
// ═══════════════════════════════════════════════════════════════════════════

static TaskHandle_t      s_workerHandle = NULL;
static QueueHandle_t     s_cmdQueue     = NULL;
static SemaphoreHandle_t s_doneSem      = NULL;

static volatile SavedCapturesPhase s_phase = SC_PHASE_IDLE;

static int s_fileIndex = -1;   // which file is open (-1 at FILE_LIST)

// File list — populated on start; survives back-nav from FILE_VIEW.
static char  s_fileBuf[SC_MAX_FILES][SC_FILE_LINE_LEN];
static const char *s_filePtrs[SC_MAX_FILES];
static char  s_fileNames[SC_MAX_FILES][SC_FILE_LINE_LEN];  // raw basenames
static uint32_t s_fileSizes[SC_MAX_FILES];
static int   s_fileCount = 0;

// File view — scrollable text buffer.
static char  s_viewBuf[SC_MAX_VIEW_LINES][SC_VIEW_LINE_LEN];
static const char *s_viewPtrs[SC_MAX_VIEW_LINES];
static int   s_viewCount = 0;

static char  s_currentTitle[32] = "Saved Captures";

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
// FILE LIST — scan /eapol
// ═══════════════════════════════════════════════════════════════════════════

static void scanDir() {
    s_fileCount = 0;

    if (!storageAcquire()) return;

    if (!storageFilesystem().exists(SC_DIR)) {
        storageRelease();
        return;
    }

    File d = storageFilesystem().open(SC_DIR);
    if (!d || !d.isDirectory()) {
        if (d) d.close();
        storageRelease();
        return;
    }

    File entry;
    while ((entry = d.openNextFile()) && s_fileCount < SC_MAX_FILES) {
        if (entry.isDirectory()) { entry.close(); continue; }

        const char *name = entry.name();
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;

        strncpy(s_fileNames[s_fileCount], base, SC_FILE_LINE_LEN - 1);
        s_fileNames[s_fileCount][SC_FILE_LINE_LEN - 1] = '\0';
        s_fileSizes[s_fileCount] = (uint32_t)entry.size();
        s_fileCount++;
        entry.close();
    }
    d.close();
    storageRelease();
}

static void formatSize(uint32_t bytes, char *out, int outLen) {
    if (bytes < 1024)             snprintf(out, outLen, "%luB", (unsigned long)bytes);
    else if (bytes < 1024UL * 1024) snprintf(out, outLen, "%luKB", (unsigned long)((bytes + 512) / 1024));
    else                           snprintf(out, outLen, "%luMB", (unsigned long)((bytes + 524288UL) / (1024UL * 1024)));
}

static void formatFileLines() {
    for (int i = 0; i < s_fileCount; i++) {
        // Trim .hc22000 / .pcap / .txt suffix for density.
        char nm[SC_FILE_LINE_LEN];
        strncpy(nm, s_fileNames[i], sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = '\0';
        int l = strlen(nm);
        if (l > 8 && strcasecmp(nm + l - 8, ".hc22000") == 0) nm[l - 8] = '\0';
        else if (l > 5 && strcasecmp(nm + l - 5, ".pcap") == 0) nm[l - 5] = '\0';
        else if (l > 4 && strcasecmp(nm + l - 4, ".txt") == 0) nm[l - 4] = '\0';

        char sz[12];
        formatSize(s_fileSizes[i], sz, sizeof(sz));
        snprintf(s_fileBuf[i], SC_FILE_LINE_LEN, "%s (%s)", nm, sz);
        s_filePtrs[i] = s_fileBuf[i];
    }
    if (s_fileCount == 0) {
        snprintf(s_fileBuf[0], SC_FILE_LINE_LEN,
                 storageIsCardPresent() ? "(no captures)" : "(storage offline)");
        s_filePtrs[0] = s_fileBuf[0];
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// FILE VIEW — read the file and wrap into fixed-width lines
// ═══════════════════════════════════════════════════════════════════════════

static void appendViewLine(const char *src, int len) {
    if (s_viewCount >= SC_MAX_VIEW_LINES) return;
    if (len > SC_VIEW_LINE_LEN - 1) len = SC_VIEW_LINE_LEN - 1;
    memcpy(s_viewBuf[s_viewCount], src, len);
    s_viewBuf[s_viewCount][len] = '\0';
    s_viewPtrs[s_viewCount] = s_viewBuf[s_viewCount];
    s_viewCount++;
}

static void loadFile(int fileIdx) {
    s_viewCount = 0;
    if (fileIdx < 0 || fileIdx >= s_fileCount) return;

    if (!storageAcquire()) return;

    char path[SC_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", SC_DIR, s_fileNames[fileIdx]);

    File f = storageFilesystem().open(path, FILE_READ);
    if (!f) {
        storageRelease();
        appendViewLine("<open failed>", 13);
        return;
    }

    // .pcap files are binary — show magic + a terse "binary N bytes" line
    // rather than dumping garbage.  .hc22000 is plain-text, falls through.
    const char *name = s_fileNames[fileIdx];
    int nameLen = strlen(name);
    bool isPcap = (nameLen > 5 && strcasecmp(name + nameLen - 5, ".pcap") == 0);
    if (isPcap) {
        uint8_t magic[4] = {0};
        f.read(magic, 4);
        char line[SC_VIEW_LINE_LEN];
        snprintf(line, sizeof(line), "pcap magic: %02X %02X %02X %02X",
                 magic[0], magic[1], magic[2], magic[3]);
        appendViewLine(line, strlen(line));
        snprintf(line, sizeof(line), "size: %lu bytes",
                 (unsigned long)s_fileSizes[fileIdx]);
        appendViewLine(line, strlen(line));
        appendViewLine("(binary - view on PC)", 21);
        f.close();
        storageRelease();
        return;
    }

    // Wrap long lines at SC_VIEW_LINE_LEN - 1; break on '\n'; swallow '\r'.
    char line[SC_VIEW_LINE_LEN];
    int li = 0;
    while (f.available() && s_viewCount < SC_MAX_VIEW_LINES) {
        char c = (char)f.read();
        if (c == '\r') continue;
        if (c == '\n') {
            appendViewLine(line, li);
            li = 0;
            continue;
        }
        if (li >= SC_VIEW_LINE_LEN - 1) {
            appendViewLine(line, li);
            li = 0;
            if (s_viewCount >= SC_MAX_VIEW_LINES) break;
        }
        line[li++] = c;
    }
    if (li > 0 && s_viewCount < SC_MAX_VIEW_LINES) {
        appendViewLine(line, li);
    }

    f.close();
    storageRelease();

    if (s_viewCount == 0) {
        appendViewLine("<empty file>", 12);
    } else if (s_viewCount >= SC_MAX_VIEW_LINES) {
        const char *trunc = "... [truncated]";
        strncpy(s_viewBuf[SC_MAX_VIEW_LINES - 1], trunc, SC_VIEW_LINE_LEN - 1);
        s_viewBuf[SC_MAX_VIEW_LINES - 1][SC_VIEW_LINE_LEN - 1] = '\0';
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// WORKER TASK
// ═══════════════════════════════════════════════════════════════════════════

static void workerTask(void *param) {
    (void)param;

    // Initial directory scan — synchronous SD read; worker flips IDLE→FILE_LIST.
    strncpy(s_currentTitle, "Saved Captures", sizeof(s_currentTitle) - 1);
    s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
    scanDir();
    formatFileLines();
    s_phase = SC_PHASE_FILE_LIST;
    postScanComplete();

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, portMAX_DELAY) != pdTRUE)
            continue;

        if (cmd.type == CMD_STOP) {
            break;
        } else if (cmd.type == SC_CMD_LOAD_FILE) {
            s_fileIndex = (int)cmd.intArg;
            postStatus("Loading...");
            loadFile(s_fileIndex);
            if (s_fileIndex >= 0 && s_fileIndex < s_fileCount) {
                char tmp[sizeof(s_currentTitle)];
                strncpy(tmp, s_fileNames[s_fileIndex], sizeof(tmp) - 1);
                tmp[sizeof(tmp) - 1] = '\0';
                int l = strlen(tmp);
                if (l > 8 && strcasecmp(tmp + l - 8, ".hc22000") == 0) tmp[l - 8] = '\0';
                else if (l > 5 && strcasecmp(tmp + l - 5, ".pcap") == 0) tmp[l - 5] = '\0';
                else if (l > 4 && strcasecmp(tmp + l - 4, ".txt") == 0) tmp[l - 4] = '\0';
                strncpy(s_currentTitle, tmp, sizeof(s_currentTitle) - 1);
                s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
            }
            s_phase = SC_PHASE_FILE_VIEW;
            postScanComplete();
        }
    }

    xSemaphoreGive(s_doneSem);
    vTaskDelete(NULL);
}

// ═══════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ═══════════════════════════════════════════════════════════════════════════

void savedCapturesStart() {
    if (s_cmdQueue) return;    // already started

    s_phase       = SC_PHASE_SCANNING;    // transient — worker flips to FILE_LIST
    s_fileIndex   = -1;
    s_fileCount   = 0;
    s_viewCount   = 0;

    s_cmdQueue = messagingCreateCmdQueue();
    s_doneSem  = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(workerTask, "saved_caps", 6144,
                            NULL, 2, &s_workerHandle, 0);
}

void savedCapturesStop() {
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
    s_phase        = SC_PHASE_IDLE;
}

SavedCapturesPhase savedCapturesGetPhase() { return s_phase; }

int savedCapturesGetFileListCount() {
    return (s_fileCount > 0) ? s_fileCount : 1;
}
bool savedCapturesHasFiles() { return s_fileCount > 0; }
const char *const *savedCapturesGetFileListLines() { return s_filePtrs; }

int savedCapturesGetFileViewCount() {
    return (s_viewCount > 0) ? s_viewCount : 1;
}
const char *const *savedCapturesGetFileViewLines() { return s_viewPtrs; }

const char *savedCapturesGetTitle() { return s_currentTitle; }

void savedCapturesSelectFile(int index) {
    if (!s_cmdQueue || s_fileCount <= 0 ||
        index < 0 || index >= s_fileCount) return;
    s_phase = SC_PHASE_LOADING;
    Cmd cmd;
    cmd.type   = SC_CMD_LOAD_FILE;
    cmd.intArg = index;
    xQueueSend(s_cmdQueue, &cmd, 0);
}

void savedCapturesBack() {
    if (s_phase == SC_PHASE_FILE_VIEW) {
        s_viewCount = 0;
        s_phase = SC_PHASE_FILE_LIST;
        strncpy(s_currentTitle, "Saved Captures", sizeof(s_currentTitle) - 1);
        s_currentTitle[sizeof(s_currentTitle) - 1] = '\0';
    }
}
