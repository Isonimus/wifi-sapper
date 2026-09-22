/**
 * @file submitted_store_littlefs.cpp
 * @brief LittleFS backing of the SubmittedStore seam (ADR-0065 gate C). Device-only.
 */
#include "net/submitted_store_littlefs.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdint>
#include <cstring>

namespace sapper {
namespace {

constexpr const char* kSubmittedPath = "/submitted.bin";
constexpr const char* kSubmittedTempPath = "/submitted.tmp";

/// File header: 4 magic bytes + 2 count bytes (LE). No fresh flag (submitted has none; ADR-0065 #1).
constexpr size_t kHeaderLen = 6;

/// Per-record on-disk layout: BSSID(6) + uploadedAt(4, uint32 LE) = 10 bytes.
constexpr size_t kRecordLen = 10;

void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

uint16_t readU16LE(const uint8_t* buf) {
    return static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
}

void writeU32LE(uint8_t* buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>(val & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

uint32_t readU32LE(const uint8_t* buf) {
    return static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) |
           (static_cast<uint32_t>(buf[2]) << 16) | (static_cast<uint32_t>(buf[3]) << 24);
}

}  // namespace

bool LittleFsSubmittedStore::begin() {
    // Silent on failure like the capture/cracked stores' begin(): the caller (hunt_loop) logs the one
    // [FATAL]. LittleFS.begin is idempotent — another store may already have mounted this filesystem.
    if (!LittleFS.begin(/*formatOnFail=*/true)) return false;
    mounted_ = true;
    return true;
}

bool LittleFsSubmittedStore::load(SubmittedEntry* out, size_t capacity, size_t& count) {
    if (!mounted_) {
        Serial.println("[ERROR] submitted store not mounted");
        return false;
    }

    // First-boot: no file yet — not an error, just an empty set.
    if (!LittleFS.exists(kSubmittedPath)) {
        count = 0;
        return true;
    }

    File f = LittleFS.open(kSubmittedPath, "r");
    if (!f) {
        Serial.println("[ERROR] submitted set open failed");
        return false;
    }

    uint8_t headerBuf[kHeaderLen];
    if (f.read(headerBuf, sizeof(headerBuf)) != sizeof(headerBuf)) {
        Serial.println("[ERROR] submitted set short header");
        f.close();
        return false;
    }
    if (headerBuf[0] != 'S' || headerBuf[1] != 'U' || headerBuf[2] != 'B' || headerBuf[3] != '1') {
        Serial.println("[ERROR] submitted set bad magic");
        f.close();
        return false;
    }
    const uint16_t recordCount = readU16LE(&headerBuf[4]);
    if (recordCount > capacity) {
        Serial.println("[ERROR] submitted set overrun");
        f.close();
        return false;
    }

    for (uint16_t i = 0; i < recordCount; ++i) {
        uint8_t buf[kRecordLen];
        if (f.read(buf, kRecordLen) != kRecordLen) {
            Serial.println("[ERROR] submitted set short record");
            f.close();
            return false;
        }
        std::memcpy(out[i].bssid, buf, 6);
        out[i].uploadedAt = readU32LE(&buf[6]);
    }

    f.close();
    count = recordCount;
    return true;
}

bool LittleFsSubmittedStore::save(const SubmittedEntry* entries, size_t count) {
    if (!mounted_) {
        Serial.println("[ERROR] submitted store not mounted");
        return false;
    }
    if (count > 65535) {
        Serial.println("[ERROR] submitted set count overflow");
        return false;
    }

    File f = LittleFS.open(kSubmittedTempPath, "w");
    if (!f) {
        Serial.println("[ERROR] submitted set temp open failed");
        return false;
    }

    uint8_t headerBuf[kHeaderLen];
    headerBuf[0] = 'S';
    headerBuf[1] = 'U';
    headerBuf[2] = 'B';
    headerBuf[3] = '1';
    writeU16LE(&headerBuf[4], static_cast<uint16_t>(count));
    if (f.write(headerBuf, sizeof(headerBuf)) != sizeof(headerBuf)) {
        Serial.println("[ERROR] submitted set temp header write failed");
        f.close();
        LittleFS.remove(kSubmittedTempPath);
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        uint8_t buf[kRecordLen];
        std::memcpy(buf, entries[i].bssid, 6);
        writeU32LE(&buf[6], entries[i].uploadedAt);
        if (f.write(buf, kRecordLen) != kRecordLen) {
            Serial.println("[ERROR] submitted set temp record write failed");
            f.close();
            LittleFS.remove(kSubmittedTempPath);
            return false;
        }
    }

    f.close();

    // Atomically replace the old set by rename alone (see LittleFsCrackedStore::save for the full
    // rationale): renaming straight over the target keeps the prior set intact on any failure, so a
    // power loss mid-write never leaves /submitted.bin gone. We only drop the temp on a failed rename.
    if (!LittleFS.rename(kSubmittedTempPath, kSubmittedPath)) {
        Serial.println("[ERROR] submitted set rename failed");
        LittleFS.remove(kSubmittedTempPath);
        return false;
    }

    return true;
}

}  // namespace sapper

#endif  // UNIT_TEST
