// SkeletonKey BLE client and light control. The worker owns every remote
// pointer and radio operation; UI consumes an immutable copied snapshot.
#include "ble_skeletonkey.h"
#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteDescriptor.h>
#include <WiFi.h>
#include <SD.h>
#include <atomic>
#include "ble_database.h"
#include "messaging.h"
#include "radio_manager.h"
#include "storage_manager.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

using namespace SkeletonKeyProtocol;
static_assert(ProfileCount <= 32, "profile mask is 32 bits");
enum Operation { Read, Rescan, Control, SelectProfile };
struct Request {
    Operation op;
    int index;
    uint32_t generation;
    uint8_t r, g, b, level;
};
struct Characteristic {
    BLERemoteCharacteristic *remote;
    uint16_t service;
    uint16_t uuid;
    int row;
};
static Characteristic s_chars[SKELETONKEY_MAX_CHARS];
static int s_charCount;
static BLERemoteCharacteristic *s_endpoints[ProfileCount];
static BLEClient *s_client;
static TaskHandle_t s_workerHandle;
static QueueHandle_t s_cmdQueue;
static SemaphoreHandle_t s_doneSem, s_snapshotMutex;
static std::atomic<bool> s_stop{false}, s_stopped{true};
static std::atomic<uint32_t> s_generation{0};
static std::atomic<SkeletonKeyState> s_state{SK_STATE_FAILED};
static SkeletonKeySnapshot s_work, s_snapshot;
static uint8_t s_targetMac[6], s_targetAddrType;
static char s_targetName[32], s_dumpPath[48];
static bool s_dumpReady;
static std::atomic<int> s_writeStatus{-1};
static std::atomic<uint16_t> s_writeHandle{0}, s_writeConn{0};
static std::atomic<int> s_writeInterface{-1};
static bool s_writeTimedOut;
// Diagnostics for "connects, then drops" reports against real hardware we
// can't test locally (e.g. ELK-BLEDOM clones): the GAP/GATT disconnect
// reason code plus how long the link actually stayed up, surfaced in both
// the UI status line and Serial so a field report carries real evidence
// instead of just "it disconnected".
static std::atomic<int> s_disconnectReason{-1};
static uint32_t s_connectedAtMs;
// Keepalive: some of these cheap BLE light controllers run their own
// idle-disconnect timer and drop the link if the app that connected never
// sends anything for a while. Re-sending the last user-fired command at a
// modest interval is the standard mitigation (used by community tools for
// this same device family) — only once a real command exists, so a dropped
// connection that occurs before any button press isn't masked by this.
static constexpr uint32_t SK_KEEPALIVE_MS = 8000;
static uint8_t s_lastBytes[PacketSize];
static size_t s_lastLen;
static uint32_t s_lastWriteMs;

static void publish(const char *status = nullptr) {
    if (status) snprintf(s_work.status, sizeof(s_work.status), "%s", status);
    s_work.state = s_state.load();
    xSemaphoreTake(s_snapshotMutex, portMAX_DELAY);
    s_snapshot = s_work;
    xSemaphoreGive(s_snapshotMutex);
    RenderMsg msg = {};
    msg.kind = RENDER_MSG_SCAN_COMPLETE;
    messagingPostTelemetry(msg);
}
static uint16_t shortUuid(BLEUUID uuid) {
    if (uuid.bitSize() == 16) return uuid.getNative()->uuid.uuid16;
    std::string text = uuid.toString();
    if (text.size() == 36 && text.compare(0, 4, "0000") == 0 &&
        text.compare(8, 28, "-0000-1000-8000-00805f9b34fb") == 0)
        return strtoul(text.substr(4,4).c_str(), nullptr, 16);
    return 0;
}
static void onGattEvent(esp_gattc_cb_event_t event, esp_gatt_if_t iface,
                        esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_WRITE_CHAR_EVT && iface == s_writeInterface.load() &&
        param->write.conn_id == s_writeConn.load() &&
        param->write.handle == s_writeHandle.load())
        s_writeStatus.store(param->write.status);
    else if (event == ESP_GATTC_DISCONNECT_EVT) {
        s_disconnectReason.store(param->disconnect.reason);
        Serial.printf("[SKELETONKEY] GATT disconnect reason=0x%02X after %lums connected\n",
                      param->disconnect.reason,
                      s_connectedAtMs ? (unsigned long)(millis() - s_connectedAtMs) : 0UL);
    }
}
// Common HCI/GATT disconnect reason codes seen from cheap BLE peripherals.
static const char *disconnectReasonText(int reason) {
    switch (reason) {
        case 0x08: return "connection timeout";
        case 0x13: return "peer terminated";
        case 0x16: return "local host terminated";
        case 0x22: return "LMP response timeout";
        case 0x3e: return "failed to establish";
        default: return "unknown reason";
    }
}
static bool saveGattDump(std::map<std::string, BLERemoteService*> *services) {
    if (!services || !storageIsCardPresent() || !storageEnsureDir("/gatt")) return false;

    // getCharacteristics() may perform blocking BLE discovery. Complete it
    // before taking the shared SD/display SPI mutex.
    for (auto &entry : *services) {
        if (entry.second) entry.second->getCharacteristics();
    }
    File file;
    if (!storageOpenOverwriteFile(s_dumpPath, &file)) return false;

    file.printf("GATT %02X:%02X:%02X:%02X:%02X:%02X\n",
                s_targetMac[0], s_targetMac[1], s_targetMac[2],
                s_targetMac[3], s_targetMac[4], s_targetMac[5]);
    for (auto &entry : *services) {
        BLERemoteService *svc = entry.second;
        if (!svc) continue;
        BLEUUID uuid = svc->getUUID();
        std::string uuidText = uuid.toString();
        const char *known = uuid.bitSize() == 16
            ? lookupServiceName(uuid.getNative()->uuid.uuid16) : nullptr;
        file.printf("SERVICE %s%s%s\n", uuidText.c_str(),
                    known ? " " : "", known ? known : "");
        auto *chars = svc->getCharacteristicsByHandle();
        if (!chars) continue;
        for (auto &charEntry : *chars) {
            BLERemoteCharacteristic *ch = charEntry.second;
            if (!ch) continue;
            std::string charUuid = ch->getUUID().toString();
            file.printf("  CHAR %s handle=0x%04X %c%c%c%c%c\n",
                        charUuid.c_str(), ch->getHandle(),
                        ch->canRead() ? 'R' : '-',
                        ch->canWrite() ? 'W' : '-',
                        ch->canWriteNoResponse() ? 'w' : '-',
                        ch->canNotify() ? 'N' : '-',
                        ch->canIndicate() ? 'I' : '-');
            auto *descriptors = ch->getDescriptors();
            if (!descriptors) continue;
            for (auto &descEntry : *descriptors) {
                BLERemoteDescriptor *desc = descEntry.second;
                if (!desc) continue;
                std::string descUuid = desc->getUUID().toString();
                file.printf("    DESC %s\n", descUuid.c_str());
            }
        }
    }
    bool ok = file.size() > 0;
    storageCloseFile(&file);
    return ok;
}


static void disconnectClient() {
    s_writeHandle = 0;
    s_writeInterface = -1;
    memset(s_endpoints, 0, sizeof(s_endpoints));
    if (s_client) {
        if (s_client->isConnected()) s_client->disconnect();
        delete s_client;
        s_client = nullptr;
    }
}
static void connectAndWalk() {
    s_state = SK_STATE_CONNECTING;
    ++s_generation;
    memset(&s_work, 0, sizeof(s_work));
    s_work.profileIndex = -1;
    snprintf(s_work.device, sizeof(s_work.device), "%s", s_targetName);
    s_charCount = 0;
    s_dumpReady = false;
    s_writeTimedOut = false;
    s_disconnectReason = -1;
    s_connectedAtMs = 0;
    s_lastLen = 0;
    publish("Connecting...");
    disconnectClient();
    if (s_stop) return;
    s_client = BLEDevice::createClient();
    if (!s_client || !s_client->connect(BLEAddress(s_targetMac),
                                      (esp_ble_addr_type_t)s_targetAddrType)) {
        s_state = SK_STATE_FAILED;
        publish("Connection failed; A+B retry");
        return;
    }
    if (s_stop) return;
    publish("Discovering GATT...");
    auto *services = s_client->getServices();
    if (!services || services->empty() || !s_client->isConnected()) {
        s_state = SK_STATE_FAILED;
        publish("GATT discovery failed; A+B retry");
        return;
    }
    // The Predator-handoff advertised name (now threaded through via
    // skeletonKeyStart()'s `name` param) is frequently empty for exactly
    // this class of device: ble_scan.cpp's scan is deliberately passive
    // (never sends SCAN_REQ), so a name the peer only carries in its scan
    // response — common for cheap BLE gear that keeps the primary
    // advertisement short — is never captured. The standard GAP "Device
    // Name" characteristic (0x1800/0x2A00) is authoritative and doesn't
    // depend on what was broadcast, so prefer it here, before detect()
    // below gets a chance to run against whatever name we have.
    {
        // BLERemoteService::getCharacteristic(uint16_t) is declared but not
        // implemented in this library version (link error) — go through
        // BLEUUID explicitly instead.
        BLERemoteService *gapSvc = s_client->getService((uint16_t)0x1800);
        BLERemoteCharacteristic *nameChar = gapSvc ? gapSvc->getCharacteristic(BLEUUID((uint16_t)0x2A00)) : nullptr;
        if (nameChar && nameChar->canRead()) {
            std::string devName = nameChar->readValue();
            if (!devName.empty()) {
                snprintf(s_targetName, sizeof(s_targetName), "%s", devName.c_str());
                snprintf(s_work.device, sizeof(s_work.device), "%s", s_targetName);
            }
        }
        Serial.printf("[SKELETONKEY] Name for profile matching: \"%s\" (GAP svc %s)\n",
                      s_targetName, gapSvc ? "present" : "absent");
    }
    int serviceCount = 0;
    for (auto &entry : *services) {
        if (s_stop || !s_client->isConnected()) break;
        auto *svc = entry.second;
        if (!svc) continue;
        uint16_t sid = shortUuid(svc->getUUID());
        if (serviceCount < SKELETONKEY_MAX_SERVICES && s_work.rowCount < SK_MAX_ROWS) {
            int row = s_work.rowCount++;
            auto uuid = svc->getUUID().toString();
            const char *known = sid ? lookupServiceName(sid) : nullptr;
            snprintf(s_work.rows[row], SK_ROW_LEN, "[Svc] %.35s", known ? known : uuid.c_str());
            snprintf(s_work.uuids[row], 37, "%s", uuid.c_str());
        } else s_work.truncated = true;
        ++serviceCount;
        // Discover first, then use handle keys: UUID keys lose repeated UUIDs.
        svc->getCharacteristics();
        auto *chars = svc->getCharacteristicsByHandle();
        if (!chars) continue;
        for (auto &ce : *chars) {
            auto *ch = ce.second;
            if (!ch) continue;
            uint16_t cid = shortUuid(ch->getUUID());
            bool writable = ch->canWrite() || ch->canWriteNoResponse();
            // Profile endpoints are found even when the visible GATT list is full.
            if (writable) {
                for (int p = 0; p < ProfileCount; ++p) {
                    if (endpoint(p, sid, cid)) {
                        if (!s_endpoints[p]) s_endpoints[p] = ch;
                        s_work.compatibleProfiles |= uint32_t(1) << p;
                    }
                }
                int detected = detect(s_targetName, sid, cid, ch->getHandle());
                if (detected >= 0 && s_work.profileIndex < 0) s_work.profileIndex = detected;
            }
            if (s_charCount >= SKELETONKEY_MAX_CHARS || s_work.rowCount >= SK_MAX_ROWS) {
                s_work.truncated = true;
                continue;
            }
            int row = s_work.rowCount++;
            s_chars[s_charCount++] = {ch, sid, cid, row};
            uint8_t flags = (ch->canRead() ? 1 : 0) | (ch->canWrite() ? 2 : 0) |
                            (ch->canNotify() ? 4 : 0) | (ch->canIndicate() ? 8 : 0) |
                            (ch->canWriteNoResponse() ? 16 : 0);
            s_work.properties[row] = flags;
            auto uuid = ch->getUUID().toString();
            snprintf(s_work.uuids[row], 37, "%s", uuid.c_str());
            snprintf(s_work.rows[row], SK_ROW_LEN, "%.8s %c%c%c%c%c @%04X", uuid.c_str(),
                     flags & 1 ? 'R' : '-', flags & 2 ? 'W' : '-', flags & 16 ? 'w' : '-',
                     flags & 4 ? 'N' : '-', flags & 8 ? 'I' : '-', ch->getHandle());
        }
    }
    if (s_stop) return;
    if (!s_client->isConnected()) {
        s_state = SK_STATE_FAILED;
        publish("Disconnected during discovery");
        return;
    }
    s_work.lightProfile = s_work.profileIndex >= 0;
    publish("Saving GATT inventory...");
    s_dumpReady = saveGattDump(services);
    if (s_stop) return;
    if (!s_client->isConnected()) {
        s_state = SK_STATE_FAILED;
        publish("Disconnected during GATT dump");
        return;
    }
    s_state = SK_STATE_CONNECTED;
    s_connectedAtMs = millis();
    Serial.printf("[SKELETONKEY] Connected, profile=%s compatibleMask=0x%08lX rows=%d\n",
                  s_work.profileIndex >= 0 ? Profiles[s_work.profileIndex].name : "(none)",
                  (unsigned long)s_work.compatibleProfiles, s_work.rowCount);
    publish(s_work.lightProfile ? "Light controls ready" : "Unknown model; GATT / choose profile");
}
static void readRow(int row) {
    if (row < 0 || row >= s_work.rowCount) return;
    if (!s_client || !s_client->isConnected()) return;
    Characteristic *found = nullptr;
    for (int i = 0; i < s_charCount; ++i) if (s_chars[i].row == row) found = &s_chars[i];
    if (!found || !found->remote->canRead()) { publish("Selected row is not readable"); return; }
    publish("Reading...");
    std::string value = found->remote->readValue();
    char *out = s_work.values[row];
    size_t n = value.size() > 16 ? 16 : value.size();
    for (size_t i = 0; i < n; ++i) snprintf(out + i*2, 40 - i*2, "%02X", uint8_t(value[i]));
    if (!n) snprintf(out, 40, "Empty / read failed");
    else if (value.size() > n) strcat(out, "...");
    if (s_dumpReady) {
        char line[128];
        snprintf(line, sizeof(line), "READ %s @%04X len=%u preview=%s\n",
                 s_work.uuids[row], found->remote->getHandle(), unsigned(value.size()), out);
        storageAppendTextFile(s_dumpPath, line);
    }
    publish("Read complete (value is hex)");
}
static void control(const Request &cmd) {
    int index = s_work.profileIndex;
    if (index < 0 || index >= ProfileCount || !s_endpoints[index]) {
        publish("No compatible light profile selected"); return;
    }
    if (s_writeTimedOut) { publish("Write timed out; reconnect first"); return; }
    auto *ch = s_endpoints[index];
    if (!s_client || !s_client->isConnected()) return;
    uint8_t bytes[PacketSize];
    size_t n = packet(index, Action(cmd.index), cmd.r, cmd.g, cmd.b, cmd.level, bytes, sizeof(bytes));
    if (!n) { publish("Command unavailable for this profile"); return; }
    memcpy(s_lastBytes, bytes, n);
    s_lastLen = n;
    s_lastWriteMs = millis();
    bool response = !ch->canWriteNoResponse();
    s_writeConn = s_client->getConnId();
    s_writeHandle = ch->getHandle();
    s_writeInterface = s_client->getGattcIf();
    s_writeStatus = -1;
    publish("Sending command...");
    esp_err_t err = esp_ble_gattc_write_char(s_client->getGattcIf(), s_client->getConnId(),
        ch->getHandle(), n, bytes, response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        char status[48]; snprintf(status, sizeof(status), "Write rejected: 0x%X", unsigned(err));
        publish(status); return;
    }
    uint32_t started = millis();
    while (!s_stop && s_client->isConnected() && s_writeStatus.load() < 0 && millis()-started < 3000)
        vTaskDelay(pdMS_TO_TICKS(10));
    int status = s_writeStatus.load();
    s_writeHandle = 0;
    if (s_stop) return;
    const char *names[] = {"ON", "OFF", "RED", "GREEN", "BLUE", "WHITE", "RGB", "BRIGHTNESS"};
    char message[48];
    if (!s_client->isConnected()) snprintf(message, sizeof(message), "Disconnected; command unconfirmed");
    else if (status < 0) {
        s_writeTimedOut = true;
        snprintf(message, sizeof(message), "Write timeout; A+B reconnect");
    } else if (status != ESP_GATT_OK) snprintf(message, sizeof(message), "GATT write error: 0x%02X", status);
    else snprintf(message, sizeof(message), "%s: %s%s", response ? "ACK" : "Sent", names[cmd.index],
                  response ? "" : " (unconfirmed)");
    if (s_dumpReady) {
        char line[128]; size_t pos = snprintf(line, sizeof(line), "WRITE %s @%04X ", Profiles[index].name, ch->getHandle());
        for (size_t i = 0; i < n; ++i) pos += snprintf(line+pos, sizeof(line)-pos, "%02X", bytes[i]);
        snprintf(line+pos, sizeof(line)-pos, " %s\n", message);
        storageAppendTextFile(s_dumpPath, line);
    }
    publish(message);
    vTaskDelay(pdMS_TO_TICKS(100)); // give inexpensive controllers time to apply a command
}
static void workerTask(void *) {
    WiFi.mode(WIFI_OFF);
    BLEDevice::init("");
    BLEDevice::setCustomGattcHandler(onGattEvent);
    connectAndWalk();
    while (!s_stop) {
        Request cmd;
        if (xQueueReceive(s_cmdQueue, &cmd, pdMS_TO_TICKS(50)) == pdTRUE && !s_stop) {
            if (cmd.generation != s_generation.load()) continue;
            if (cmd.op == Rescan) connectAndWalk();
            else if (s_state == SK_STATE_CONNECTED && s_client && s_client->isConnected()) {
                if (cmd.op == Read) readRow(cmd.index);
                else if (cmd.op == Control) control(cmd);
                else if (cmd.op == SelectProfile && cmd.index >= 0 && cmd.index < ProfileCount && s_endpoints[cmd.index]) {
                    s_work.profileIndex = cmd.index; s_work.lightProfile = true;
                    publish("Profile selected; no command sent");
                }
            }
        }
        if (s_state == SK_STATE_CONNECTED && s_client && s_client->isConnected() &&
            s_lastLen && millis() - s_lastWriteMs >= SK_KEEPALIVE_MS) {
            // Some cheap BLE light controllers (ELK-BLEDOM clones and
            // similar) run their own idle-disconnect timer and drop the
            // link if nothing is written for a while. Re-send the last
            // user-fired command as a no-visible-effect keepalive; only
            // armed once a real command exists, so a drop that happens
            // before the user ever presses anything isn't masked by this.
            int idx = s_work.profileIndex;
            if (idx >= 0 && idx < ProfileCount && s_endpoints[idx]) {
                auto *ch = s_endpoints[idx];
                esp_ble_gattc_write_char(s_client->getGattcIf(), s_client->getConnId(), ch->getHandle(),
                    s_lastLen, s_lastBytes,
                    ch->canWriteNoResponse() ? ESP_GATT_WRITE_TYPE_NO_RSP : ESP_GATT_WRITE_TYPE_RSP,
                    ESP_GATT_AUTH_REQ_NONE);
            }
            s_lastWriteMs = millis();
        }
        if (s_state == SK_STATE_CONNECTED && (!s_client || !s_client->isConnected())) {
            s_state = SK_STATE_FAILED;
            int reason = s_disconnectReason.load();
            unsigned long upMs = s_connectedAtMs ? (unsigned long)(millis() - s_connectedAtMs) : 0;
            char msg[48];
            if (reason >= 0) snprintf(msg, sizeof(msg), "Disconnected (%s) after %lus",
                                      disconnectReasonText(reason), upMs / 1000);
            else snprintf(msg, sizeof(msg), "Disconnected after %lus; A+B reconnect", upMs / 1000);
            Serial.printf("[SKELETONKEY] %s\n", msg);
            publish(msg);
        }
    }
    disconnectClient();
    BLEDevice::setCustomGattcHandler(nullptr);
    BLEDevice::deinit(false);
    radioRelease(RADIO_BLE);
    s_state = SK_STATE_FAILED;
    xSemaphoreGive(s_doneSem);
    s_stopped = true;
    vTaskDelete(nullptr);
}
void skeletonKeyStart(const uint8_t *mac, uint8_t addrType, const char *name) {
    if (s_cmdQueue) return;
    if (!s_snapshotMutex) s_snapshotMutex = xSemaphoreCreateMutex();
    if (!s_snapshotMutex) { s_state = SK_STATE_FAILED; return; }
    memset(&s_work, 0, sizeof(s_work)); s_work.profileIndex = -1;
    s_state = SK_STATE_FAILED;
    if (!mac || !radioAcquire(RADIO_BLE)) { publish("BLE radio unavailable"); return; }
    memcpy(s_targetMac, mac, 6); s_targetAddrType = addrType;
    if (name && name[0]) snprintf(s_targetName, sizeof(s_targetName), "%s", name);
    else snprintf(s_targetName, sizeof(s_targetName), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_dumpPath, sizeof(s_dumpPath), "/gatt/gatt_%02X%02X%02X%02X%02X%02X.txt",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_cmdQueue = xQueueCreate(4, sizeof(Request));
    s_doneSem = xSemaphoreCreateBinary();
    s_stop = false; s_stopped = false; s_state = SK_STATE_CONNECTING;
    snprintf(s_work.device, sizeof(s_work.device), "%s", s_targetName);
    publish("Connecting...");
    if (!s_cmdQueue || !s_doneSem ||
        xTaskCreatePinnedToCore(workerTask, "skeletonkey", 8192, nullptr, 2, &s_workerHandle, 0) != pdPASS) {
        if (s_cmdQueue) vQueueDelete(s_cmdQueue);
        if (s_doneSem) vSemaphoreDelete(s_doneSem);
        s_cmdQueue = nullptr; s_doneSem = nullptr; s_workerHandle = nullptr;
        s_stopped = true; s_state = SK_STATE_FAILED; radioRelease(RADIO_BLE);
        publish("Not enough memory to start BLE");
    }
}
void skeletonKeyRequestStop() { s_stop = true; }
bool skeletonKeyIsStopped() { return s_stopped.load(); }
void skeletonKeyStop() {
    if (!s_cmdQueue) return;
    s_stop = true;
    xSemaphoreTake(s_doneSem, portMAX_DELAY);
    // The worker has no further access to either RTOS object after signaling.
    vQueueDelete(s_cmdQueue); vSemaphoreDelete(s_doneSem);
    s_cmdQueue = nullptr; s_doneSem = nullptr; s_workerHandle = nullptr;
    s_stopped = true;
}
SkeletonKeyState skeletonKeyGetState() { return s_state.load(); }
void skeletonKeyGetSnapshot(SkeletonKeySnapshot *out) {
    if (!out) return;
    if (!s_snapshotMutex) { memset(out, 0, sizeof(*out)); out->state = SK_STATE_FAILED; out->profileIndex = -1; return; }
    xSemaphoreTake(s_snapshotMutex, portMAX_DELAY); *out = s_snapshot; xSemaphoreGive(s_snapshotMutex);
}
// Compatibility with the older T-Embed/Cardputer read-only screens.
static SkeletonKeySnapshot s_legacy;
static const char *s_legacyRows[SK_MAX_ROWS];
const char *const *skeletonKeyGetRowLines() {
    skeletonKeyGetSnapshot(&s_legacy);
    for (int i = 0; i < s_legacy.rowCount; ++i) s_legacyRows[i] = s_legacy.rows[i];
    return s_legacyRows;
}
int skeletonKeyGetRowCount() {
    if (!s_snapshotMutex) return 0;
    xSemaphoreTake(s_snapshotMutex, portMAX_DELAY); int n = s_snapshot.rowCount; xSemaphoreGive(s_snapshotMutex); return n;
}
static bool enqueue(Request cmd) {
    if (!s_cmdQueue || s_stop) return false;
    cmd.generation = s_generation.load();
    return xQueueSend(s_cmdQueue, &cmd, 0) == pdTRUE;
}
void skeletonKeyRequestRead(int row) { enqueue({Read, row, 0, 0, 0, 0, 0}); }
void skeletonKeyRequestRescan() { enqueue({Rescan, 0, 0, 0, 0, 0, 0}); }
bool skeletonKeyRequestControl(Action action, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
    if (s_state != SK_STATE_CONNECTED || action > Brightness) return false;
    return enqueue({Control, int(action), 0, r, g, b, level});
}
bool skeletonKeyRequestProfile(int index) {
    return s_state == SK_STATE_CONNECTED && enqueue({SelectProfile, index, 0, 0, 0, 0, 0});
}
