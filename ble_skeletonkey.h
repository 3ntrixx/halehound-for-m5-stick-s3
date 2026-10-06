#ifndef BLE_SKELETONKEY_H
#define BLE_SKELETONKEY_H
#include <Arduino.h>
#include "skeletonkey_protocol.h"

#define SKELETONKEY_MAX_SERVICES 16
#define SKELETONKEY_MAX_CHARS 48
#define SK_MAX_ROWS (SKELETONKEY_MAX_SERVICES + SKELETONKEY_MAX_CHARS)
#define SK_ROW_LEN 48

enum SkeletonKeyState { SK_STATE_CONNECTING, SK_STATE_CONNECTED, SK_STATE_FAILED };

// All BLE operations run on one worker. Names are copied before scan teardown.
void skeletonKeyStart(const uint8_t *mac, uint8_t addrType, const char *name = nullptr);
void skeletonKeyStop(); // blocking compatibility API for other board ports
void skeletonKeyRequestStop();
bool skeletonKeyIsStopped();
SkeletonKeyState skeletonKeyGetState();
const char *const *skeletonKeyGetRowLines();
int skeletonKeyGetRowCount();
void skeletonKeyRequestRead(int rowIndex);
void skeletonKeyRequestRescan();
bool skeletonKeyRequestControl(SkeletonKeyProtocol::Action action,
                               uint8_t r = 0, uint8_t g = 0, uint8_t b = 0,
                               uint8_t level = 100);
bool skeletonKeyRequestProfile(int profileIndex);

// Copy under a mutex; renderer never borrows worker-owned mutable strings.
struct SkeletonKeySnapshot {
    SkeletonKeyState state;
    bool lightProfile;
    bool truncated;
    int profileIndex;
    uint32_t compatibleProfiles; // bit per SkeletonKeyProtocol::Profiles entry
    int rowCount;
    char device[32];
    char status[48];
    char rows[SK_MAX_ROWS][SK_ROW_LEN];
    char uuids[SK_MAX_ROWS][37];
    char values[SK_MAX_ROWS][40];
    uint8_t properties[SK_MAX_ROWS]; // R=1, W=2, N=4, I=8, write-no-response=16
};
void skeletonKeyGetSnapshot(SkeletonKeySnapshot *out);
// Complete inventory (including entries beyond the UI limit) is saved to
// /gatt/gatt_<MAC>.txt on SD or StickS3 LittleFS when storage is available.
#endif
