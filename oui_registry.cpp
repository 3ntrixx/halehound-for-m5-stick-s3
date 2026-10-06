#include "oui_registry.h"

#include <SD.h>
#include <string.h>

#include "spi_manager.h"
#include "storage_manager.h"

namespace {
constexpr char kPath[] = "/oui.bin";
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRecordSize = 24;
constexpr uint32_t kHeaderSize = 12;
constexpr uint32_t kMaxRecords = 0xFFFFFF;
}

bool ouiRegistryLookup(const uint8_t mac[6], char *name, size_t nameSize) {
    if (!mac || !name || nameSize == 0) return false;
    name[0] = '\0';
    // Locally administered addresses do not carry a manufacturer OUI.
    if ((mac[0] & 0x03) != 0 || !storageIsCardPresent()) return false;
    if (!spiSelectSD()) return false;

    bool found = false;
    File file = SD.open(kPath, FILE_READ);
    if (file) {
        uint8_t header[kHeaderSize];
        if (file.read(header, sizeof(header)) == sizeof(header) &&
            memcmp(header, "HHOU", 4) == 0 &&
            header[4] == kVersion && header[5] == kRecordSize &&
            header[6] == 0 && header[7] == 0) {
            uint32_t count = (uint32_t)header[8] |
                             ((uint32_t)header[9] << 8) |
                             ((uint32_t)header[10] << 16) |
                             ((uint32_t)header[11] << 24);
            if (count > 0 && count <= kMaxRecords &&
                (uint64_t)kHeaderSize + (uint64_t)count * kRecordSize == file.size()) {
                uint32_t lo = 0, hi = count;
                uint8_t record[kRecordSize];
                while (lo < hi) {
                    uint32_t mid = lo + (hi - lo) / 2;
                    if (!file.seek(kHeaderSize + mid * kRecordSize) ||
                        file.read(record, sizeof(record)) != sizeof(record)) break;
                    int comparison = memcmp(mac, record, 3);
                    if (comparison == 0) {
                        // A corrupt record must not leak into the UI.
                        if (memchr(record + 3, '\0', kRecordSize - 3) != nullptr) {
                            size_t n = strnlen((const char *)record + 3, kRecordSize - 3);
                            if (n >= nameSize) n = nameSize - 1;
                            memcpy(name, record + 3, n);
                            name[n] = '\0';
                            found = n != 0;
                        }
                        break;
                    }
                    if (comparison < 0) hi = mid;
                    else lo = mid + 1;
                }
            }
        }
        file.close();
    }
    spiDeselect();
    return found;
}
