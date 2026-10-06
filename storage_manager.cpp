// ═══════════════════════════════════════════════════════════════════════════
// HaleHound Storage Manager — implementation
// Created: 2026-09-17 (T-Embed CC1101 port, roadmap §7 phase 6)
// ═══════════════════════════════════════════════════════════════════════════

#include "storage_manager.h"
#include <SD.h>
#ifdef BOARD_M5STICK_S3
#include <LittleFS.h>
#include <esp_partition.h>
#endif
#include <SPI.h>
#include "cyd_config.h"
#include "messaging.h"
#include "psram_pool.h"
#include "spi_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *const FILE_PATHS[STORAGE_FILE_COUNT] = {
#ifdef BOARD_M5STICK_S3
    "/wardriving/wardrive_lite.csv",
#else
    "/wardrive_lite.csv",
#endif
};

static bool s_cardPresent = false;
static TaskHandle_t s_storageTaskHandle = NULL;
#ifdef BOARD_M5STICK_S3
static SemaphoreHandle_t s_fsMutex = NULL;

static bool filesystemPartitionIsErased() {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (!partition) return false;
    uint8_t block[1024];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
        size_t count = partition->size - offset;
        if (count > sizeof(block)) count = sizeof(block);
        if (esp_partition_read(partition, offset, block, count) != ESP_OK)
            return false;
        for (size_t i = 0; i < count; ++i)
            if (block[i] != 0xFF) return false;
    }
    return true;
}
#endif

static fs::FS &storageFs() {
#ifdef BOARD_M5STICK_S3
    return LittleFS;
#else
    return SD;
#endif
}

static bool selectStorage() {
#ifdef BOARD_M5STICK_S3
    return s_fsMutex && xSemaphoreTake(s_fsMutex, pdMS_TO_TICKS(1000)) == pdTRUE;
#else
    return spiSelect(SPI_DEVICE_SD);
#endif
}

static void deselectStorage() {
#ifdef BOARD_M5STICK_S3
    xSemaphoreGive(s_fsMutex);
#else
    spiDeselect();
#endif
}

// The storage worker alone owns the current path. Producers serialize reset
// and write submissions so a reset stays behind all previously queued writes.
static char s_captureFilePath[CAPTURE_PATH_MAX] = "";
static SemaphoreHandle_t s_captureSubmitMutex = NULL;
static SemaphoreHandle_t s_captureResetDone = NULL;
static bool s_captureReady = false; // protected by s_captureSubmitMutex
static bool s_captureResetOk = false; // published through s_captureResetDone

static bool writeToPath(const char *path, const uint8_t *data, size_t len) {
    if (!s_cardPresent || !selectStorage()) {
        return false;
    }
    bool ok = false;
    File f = storageFs().open(path, FILE_APPEND);
    if (f) {
        ok = f.write(data, len) == len;
        f.close();
    }
    #if CYD_DEBUG
    else {
        Serial.printf("[STORAGE] open failed: %s\n", path);
    }
    #endif
    deselectStorage();
    return ok;
}

static void handleLogMsg(const LogMsg &msg) {
    if (msg.fileId < STORAGE_FILE_COUNT) {
        if (!writeToPath(FILE_PATHS[msg.fileId], psramPoolData(msg.poolHandle), msg.len)) {
            droppedLogMsgs++;
        }
    }
    psramPoolRelease(msg.poolHandle);
}

static void handleCaptureMsg(const CaptureMsg &msg) {
    if (msg.kind == CAPTURE_RESET) {
        s_captureFilePath[0] = '\0';
        bool ok = false;
        if (selectStorage()) {
            ok = !storageFs().exists(msg.path) || storageFs().remove(msg.path);
            deselectStorage();
        }
        if (ok) {
            memcpy(s_captureFilePath, msg.path, sizeof(s_captureFilePath));
        }
        s_captureResetOk = ok;
        xSemaphoreGive(s_captureResetDone);
        return;
    }
    if (s_captureFilePath[0] == '\0' ||
        !writeToPath(s_captureFilePath, psramPoolData(msg.poolHandle), msg.len)) {
        droppedCaptureMsgs++;
    }
    psramPoolRelease(msg.poolHandle);
}

static void storageTask(void *param) {
    (void)param;

    for (;;) {
        LogMsg logMsg;
        if (xQueueReceive(log_q, &logMsg, pdMS_TO_TICKS(20)) == pdTRUE) {
            handleLogMsg(logMsg);
        }

        CaptureMsg capMsg;
        if (xQueueReceive(capture_q, &capMsg, pdMS_TO_TICKS(20)) == pdTRUE) {
            handleCaptureMsg(capMsg);
        }
    }
}

bool storageManagerSetup() {
    s_captureSubmitMutex = xSemaphoreCreateMutex();
    s_captureResetDone = xSemaphoreCreateBinary();
    if (!s_captureSubmitMutex || !s_captureResetDone) return false;

#ifdef BOARD_M5STICK_S3
    s_fsMutex = xSemaphoreCreateMutex();
    if (!s_fsMutex) return false;
    // Preserve an existing but unreadable filesystem. Formatting is safe only
    // when the entire data partition is still erased on first boot.
    s_cardPresent = LittleFS.begin(false);
    if (!s_cardPresent && filesystemPartitionIsErased()) {
        s_cardPresent = LittleFS.begin(true);
    }
#else
    if (spiSelect(SPI_DEVICE_SD)) {
        // spiBus() — the one SPIClass instance that owns the shared
        // display/CC1101/NRF24/SD bus on this board (TFT_eSPI's HSPI
        // instance, see User_Setup_TEmbed.h), never the framework's global
        // SPI (FSPI) object.
        s_cardPresent = SD.begin(SD_CS, spiBus(), SPI_SPEED_SD);
        spiDeselect();
    }
#endif

    #if CYD_DEBUG
    Serial.printf("[STORAGE] %s %s\n",
#ifdef BOARD_M5STICK_S3
                  "LittleFS",
#else
                  "SD card",
#endif
                  s_cardPresent ? "mounted" : "NOT FOUND");
    #endif

    if (xTaskCreatePinnedToCore(storageTask, "storage", 4096, NULL, 1,
                               &s_storageTaskHandle, 0) != pdPASS) {
        s_storageTaskHandle = NULL;
        s_cardPresent = false;
    }
    return s_cardPresent;
}

bool storageIsCardPresent() {
    return s_cardPresent;
}

bool storageAcquire() {
    return s_cardPresent && selectStorage();
}

void storageRelease() {
    deselectStorage();
}

fs::FS &storageFilesystem() {
    return storageFs();
}

bool storageLogLine(uint8_t fileId, const char *line) {
    if (!s_cardPresent || fileId >= STORAGE_FILE_COUNT || !line) {
        return false;
    }

    size_t len = strlen(line);
    if (len == 0) {
        return false;
    }
    // Normalize a supplied CRLF/LF and always leave room for one terminator.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    if (len >= psramPoolBlockSize()) len = psramPoolBlockSize() - 1;

    int32_t handle = psramPoolAcquire();
    if (handle < 0) {
        return false;
    }
    memcpy(psramPoolData(handle), line, len);
    psramPoolData(handle)[len++] = '\n';

    LogMsg msg;
    msg.fileId = fileId;
    msg.poolHandle = handle;
    msg.len = len;

    if (!messagingPostLog(msg)) {
        psramPoolRelease(handle);
        return false;
    }
    return true;
}

bool storageResetCaptureFile(const char *path) {
    if (!s_cardPresent || !s_storageTaskHandle || !s_captureSubmitMutex ||
        !path || path[0] == '\0' || strlen(path) >= CAPTURE_PATH_MAX) {
        return false;
    }

    xSemaphoreTake(s_captureSubmitMutex, portMAX_DELAY);
    s_captureReady = false;
    CaptureMsg msg = {};
    msg.kind = CAPTURE_RESET;
    msg.poolHandle = -1;
    strcpy(msg.path, path);
    // FIFO barrier: the worker completes old writes before removing the file.
    // Wait for its acknowledgement so subsequent bytes follow the reset.
    bool ok = xQueueSend(capture_q, &msg, portMAX_DELAY) == pdTRUE;
    if (ok) {
        xSemaphoreTake(s_captureResetDone, portMAX_DELAY);
        ok = s_captureResetOk;
    }
    s_captureReady = ok;
    xSemaphoreGive(s_captureSubmitMutex);
    return ok;
}

bool storageCaptureBytes(const uint8_t *data, size_t len) {
    if (!s_cardPresent || !s_captureSubmitMutex || !data || len == 0 ||
        len > psramPoolBlockSize()) {
        return false;
    }
    // Keep the producer wait bounded while a rare reset is in progress.
    if (xSemaphoreTake(s_captureSubmitMutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        droppedCaptureMsgs++;
        return false;
    }
    if (!s_captureReady) {
        xSemaphoreGive(s_captureSubmitMutex);
        return false;
    }

    int32_t handle = psramPoolAcquire();
    if (handle < 0) {
        xSemaphoreGive(s_captureSubmitMutex);
        return false;
    }
    memcpy(psramPoolData(handle), data, len);

    CaptureMsg msg = {};
    msg.kind = CAPTURE_WRITE;
    msg.poolHandle = handle;
    msg.len = len;

    bool ok = messagingPostCapture(msg);
    if (!ok) {
        psramPoolRelease(handle);
    }
    xSemaphoreGive(s_captureSubmitMutex);
    return ok;
}

bool storageEnsureDir(const char *path) {
    if (!s_cardPresent) {
        return false;
    }
    bool ok = false;
    if (selectStorage()) {
        ok = storageFs().exists(path) || storageFs().mkdir(path);
        deselectStorage();
    }
    return ok;
}

bool storageAppendTextFile(const char *path, const char *text) {
    if (!s_cardPresent || !path || !text) {
        return false;
    }
    size_t len = strlen(text);
    if (len == 0) {
        return false;
    }
    return writeToPath(path, (const uint8_t *)text, len);
}

bool storageOpenOverwriteFile(const char *path, File *file) {
    if (!s_cardPresent || !path || path[0] != '/' || !file || !selectStorage())
        return false;
    bool removed = !storageFs().exists(path) || storageFs().remove(path);
    if (removed) *file = storageFs().open(path, FILE_WRITE);
    if (!removed || !*file) {
        deselectStorage();
        return false;
    }
    return true;
}

void storageCloseFile(File *file) {
    if (!file) return;
    file->flush();
    file->close();
    deselectStorage();
}
