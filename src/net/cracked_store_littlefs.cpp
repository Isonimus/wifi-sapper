/**
 * @file cracked_store_littlefs.cpp
 * @brief LittleFS backing of the CrackedStore seam (ADR-0019 decision #4). Device-only.
 */
#include "net/cracked_store_littlefs.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdint>
#include <cstring>

namespace sapper {
namespace {

constexpr const char* kCrackedPath = "/cracked.bin";
constexpr const char* kCrackedTempPath = "/cracked.tmp";

/// Write a little-endian uint16 to a buffer.
void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

/// Read a little-endian uint16 from a buffer.
uint16_t readU16LE(const uint8_t* buf) {
    return (static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8));
}

/// Write a little-endian uint32 to a buffer.
void writeU32LE(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

/// Read a little-endian uint32 from a buffer.
uint32_t readU32LE(const uint8_t* buf) {
    return (static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
            (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24));
}

}  // namespace

bool LittleFsCrackedStore::begin() {
    // Silent on failure like the capture store's begin(): the caller (hunt_loop) logs the one [FATAL],
    // so a mount failure is not reported twice. LittleFS.begin is idempotent — the capture store may
    // already have mounted the same filesystem this run.
    if (!LittleFS.begin(/*formatOnFail=*/true)) return false;
    mounted_ = true;
    return true;
}

bool LittleFsCrackedStore::load(CrackedEntry* out, size_t capacity, size_t& count,
                                bool& freshManifest) {
    if (!mounted_) {
        Serial.println("[ERROR] cracked store not mounted");
        return false;
    }

    // First-boot: no file yet — this is not an error, just an empty manifest.
    if (!LittleFS.exists(kCrackedPath)) {
        count = 0;
        freshManifest = true;
        return true;
    }

    File f = LittleFS.open(kCrackedPath, "r");
    if (!f) {
        Serial.println("[ERROR] cracked manifest open failed");
        return false;
    }

    // Read and validate the 7-byte header.
    uint8_t headerBuf[7];
    const size_t headerRead = f.read(headerBuf, sizeof(headerBuf));
    if (headerRead != sizeof(headerBuf)) {
        Serial.println("[ERROR] cracked manifest short header");
        f.close();
        return false;
    }

    // Validate magic bytes.
    if (headerBuf[0] != 'C' || headerBuf[1] != 'R' || headerBuf[2] != 'K' || headerBuf[3] != '1') {
        Serial.println("[ERROR] cracked manifest bad magic");
        f.close();
        return false;
    }

    // Extract the freshManifest flag and count.
    freshManifest = (headerBuf[4] != 0);
    uint16_t recordCount = readU16LE(&headerBuf[5]);

    // Check for overflow.
    if (recordCount > capacity) {
        Serial.println("[ERROR] cracked manifest overrun");
        f.close();
        return false;
    }

    // Read each record field-by-field.
    for (uint16_t i = 0; i < recordCount; ++i) {
        CrackedEntry& entry = out[i];

        // Read BSSID (6 bytes).
        uint8_t buf[6];
        if (f.read(buf, 6) != 6) {
            Serial.println("[ERROR] cracked manifest short record (bssid)");
            f.close();
            return false;
        }
        std::memcpy(entry.result.bssid, buf, 6);

        // Read ESSID (33 bytes, fixed-width, NUL-terminated).
        if (f.read(reinterpret_cast<uint8_t*>(entry.result.essid), 33) != 33) {
            Serial.println("[ERROR] cracked manifest short record (essid)");
            f.close();
            return false;
        }

        // Read password (65 bytes, fixed-width, NUL-terminated).
        if (f.read(reinterpret_cast<uint8_t*>(entry.result.password), 65) != 65) {
            Serial.println("[ERROR] cracked manifest short record (password)");
            f.close();
            return false;
        }

        // Read firstSeenCrackedMs (4 bytes, uint32 LE).
        if (f.read(buf, 4) != 4) {
            Serial.println("[ERROR] cracked manifest short record (timestamp)");
            f.close();
            return false;
        }
        entry.firstSeenCrackedMs = readU32LE(buf);
    }

    f.close();
    count = recordCount;
    return true;
}

bool LittleFsCrackedStore::save(const CrackedEntry* entries, size_t count, bool freshManifest) {
    if (!mounted_) {
        Serial.println("[ERROR] cracked store not mounted");
        return false;
    }

    // Ensure count fits in uint16.
    if (count > 65535) {
        Serial.println("[ERROR] cracked manifest count overflow");
        return false;
    }

    // Write to temp file.
    File f = LittleFS.open(kCrackedTempPath, "w");
    if (!f) {
        Serial.println("[ERROR] cracked manifest temp open failed");
        return false;
    }

    // Write header: 4 magic bytes + 1 freshManifest byte + 2 count bytes (LE).
    uint8_t headerBuf[7];
    headerBuf[0] = 'C';
    headerBuf[1] = 'R';
    headerBuf[2] = 'K';
    headerBuf[3] = '1';
    headerBuf[4] = freshManifest ? 1 : 0;
    writeU16LE(&headerBuf[5], static_cast<uint16_t>(count));

    if (f.write(headerBuf, sizeof(headerBuf)) != sizeof(headerBuf)) {
        Serial.println("[ERROR] cracked manifest temp header write failed");
        f.close();
        LittleFS.remove(kCrackedTempPath);
        return false;
    }

    // Write each record field-by-field.
    for (size_t i = 0; i < count; ++i) {
        const CrackedEntry& entry = entries[i];

        // Write BSSID (6 bytes).
        if (f.write(entry.result.bssid, 6) != 6) {
            Serial.println("[ERROR] cracked manifest temp record write failed (bssid)");
            f.close();
            LittleFS.remove(kCrackedTempPath);
            return false;
        }

        // Write ESSID (33 bytes, fixed-width).
        if (f.write(reinterpret_cast<const uint8_t*>(entry.result.essid), 33) != 33) {
            Serial.println("[ERROR] cracked manifest temp record write failed (essid)");
            f.close();
            LittleFS.remove(kCrackedTempPath);
            return false;
        }

        // Write password (65 bytes, fixed-width).
        if (f.write(reinterpret_cast<const uint8_t*>(entry.result.password), 65) != 65) {
            Serial.println("[ERROR] cracked manifest temp record write failed (password)");
            f.close();
            LittleFS.remove(kCrackedTempPath);
            return false;
        }

        // Write firstSeenCrackedMs (4 bytes, uint32 LE).
        uint8_t tsBuf[4];
        writeU32LE(tsBuf, entry.firstSeenCrackedMs);
        if (f.write(tsBuf, 4) != 4) {
            Serial.println("[ERROR] cracked manifest temp record write failed (timestamp)");
            f.close();
            LittleFS.remove(kCrackedTempPath);
            return false;
        }
    }

    f.close();

    // Atomically replace the old manifest by rename alone. LittleFS.rename() overwrites an existing
    // destination atomically, so we must NOT remove the live manifest first: a remove()-then-rename()
    // opens a window where a rename failure or a power loss (the exact routine event this store survives)
    // leaves /cracked.bin gone and /cracked.tmp not yet in place — the next boot would read a missing
    // file as a fresh empty manifest and silently lose the whole history. Renaming straight over the
    // target keeps the prior manifest intact on any failure; we only drop the temp so it cannot linger.
    if (!LittleFS.rename(kCrackedTempPath, kCrackedPath)) {
        Serial.println("[ERROR] cracked manifest rename failed");
        LittleFS.remove(kCrackedTempPath);  // prior /cracked.bin is untouched; discard the temp.
        return false;
    }

    return true;
}

}  // namespace sapper

#endif  // UNIT_TEST
