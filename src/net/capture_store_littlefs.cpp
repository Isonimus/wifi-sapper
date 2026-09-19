/**
 * @file capture_store_littlefs.cpp
 * @brief LittleFS backing of the CaptureStore seam (ADR-0017 decision #3). Device-only.
 */
#include "net/capture_store_littlefs.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <LittleFS.h>

#include <cstdio>
#include <cstring>

namespace sapper {
namespace {

// One directory holds every pending pcap; names carry the id and BSSID (see the header). The id is
// 8 hex digits, the BSSID 12, joined by '_' with a ".pcap" suffix, all under kCaptureDir.
constexpr const char* kCaptureDir = "/caps";
constexpr size_t kNameLen = 8 + 1 + 12 + 5;  // "<id>_<bssid>.pcap", no NUL.
constexpr size_t kPathCap = 64;              // kCaptureDir + '/' + name + NUL, with margin.

/// Parse the 8-hex id prefix of a bare filename (no directory). Returns false if it is not the
/// expected `<8-hex>_<12-hex>.pcap` shape — a stray file is ignored, not mistaken for a capture.
bool parseName(const char* name, uint32_t& id, uint8_t bssid[6]) {
    if (std::strlen(name) != kNameLen) return false;
    if (name[8] != '_') return false;
    if (std::memcmp(name + 9 + 12, ".pcap", 5) != 0) return false;  // reject a stray non-capture file.
    char idHex[9] = {0};
    std::memcpy(idHex, name, 8);
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(idHex, &end, 16);
    if (end != idHex + 8) return false;
    id = static_cast<uint32_t>(parsed);
    for (int i = 0; i < 6; ++i) {
        char byteHex[3] = {name[9 + i * 2], name[9 + i * 2 + 1], 0};
        char* byteEnd = nullptr;
        const unsigned long b = std::strtoul(byteHex, &byteEnd, 16);
        if (byteEnd != byteHex + 2) return false;
        bssid[i] = static_cast<uint8_t>(b);
    }
    return true;
}

}  // namespace

bool LittleFsCaptureStore::formatPath(char* out, size_t cap, uint32_t id,
                                      const uint8_t bssid[6]) const {
    const int n = std::snprintf(out, cap, "%s/%08lx_%02x%02x%02x%02x%02x%02x.pcap", kCaptureDir,
                                static_cast<unsigned long>(id), bssid[0], bssid[1], bssid[2],
                                bssid[3], bssid[4], bssid[5]);
    return n > 0 && static_cast<size_t>(n) < cap;
}

bool LittleFsCaptureStore::begin() {
    if (!LittleFS.begin(/*formatOnFail=*/true)) return false;  // fail loud: no place to persist.
    LittleFS.mkdir(kCaptureDir);
    mounted_ = true;

    // Resume the id counter above the highest id on flash so a reboot never reissues an id — the
    // monotonic-id contract oldest-eviction relies on (capture_store.h).
    File dir = LittleFS.open(kCaptureDir);
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            const char* path = f.name();
            const char* base = std::strrchr(path, '/');
            base = base ? base + 1 : path;
            uint32_t id = 0;
            uint8_t bssid[6];
            if (parseName(base, id, bssid) && id >= nextId_) nextId_ = id + 1;
        }
    }
    return true;
}

bool LittleFsCaptureStore::enqueue(const uint8_t bssid[6], const uint8_t* pcap, size_t len,
                                   uint32_t& outId) {
    if (!mounted_) return false;
    const uint32_t id = nextId_;
    char path[kPathCap];
    if (!formatPath(path, sizeof(path), id, bssid)) return false;

    File f = LittleFS.open(path, "w");
    if (!f) return false;
    const size_t wrote = f.write(pcap, len);
    f.close();
    if (wrote != len) {
        LittleFS.remove(path);  // partial write is a corrupt pcap — drop it, fail loud (§3).
        return false;
    }
    nextId_ = id + 1;  // advance only after a fully-written file, so a failure does not burn an id.
    outId = id;
    return true;
}

bool LittleFsCaptureStore::listPending(PendingCapture* out, size_t capacity, size_t& count) {
    if (!mounted_) return false;
    File dir = LittleFS.open(kCaptureDir);
    if (!dir) return false;

    size_t n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* path = f.name();
        const char* base = std::strrchr(path, '/');
        base = base ? base + 1 : path;
        PendingCapture entry;
        if (!parseName(base, entry.id, entry.bssid)) continue;  // ignore non-capture files.
        if (n >= capacity) return false;  // more pending than the caller can hold — a fault (§3).
        // Insertion sort by ascending id so the list is oldest-first, the order the queue's
        // oldest-eviction depends on; the directory yields no defined order.
        size_t pos = n;
        while (pos > 0 && out[pos - 1].id > entry.id) {
            out[pos] = out[pos - 1];
            --pos;
        }
        out[pos] = entry;
        ++n;
    }
    count = n;
    return true;
}

bool LittleFsCaptureStore::findPathById(char* out, size_t cap, uint32_t id) const {
    File dir = LittleFS.open(kCaptureDir);
    if (!dir) return false;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const char* path = f.name();
        const char* base = std::strrchr(path, '/');
        base = base ? base + 1 : path;
        uint32_t fid = 0;
        uint8_t bssid[6];
        if (parseName(base, fid, bssid) && fid == id) return formatPath(out, cap, id, bssid);
    }
    return false;
}

bool LittleFsCaptureStore::read(uint32_t id, uint8_t* out, size_t capacity, size_t& len) {
    if (!mounted_) return false;
    char path[kPathCap];
    if (!findPathById(path, sizeof(path), id)) return false;  // absent.
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    const size_t size = f.size();
    if (size > capacity) {  // an oversized pcap is a fault to surface, never a truncated read (§3).
        f.close();
        return false;
    }
    const size_t got = f.read(out, size);
    f.close();
    if (got != size) return false;
    len = size;
    return true;
}

bool LittleFsCaptureStore::remove(uint32_t id) {
    if (!mounted_) return false;
    char path[kPathCap];
    if (!findPathById(path, sizeof(path), id)) return false;  // absent.
    return LittleFS.remove(path);
}

}  // namespace sapper

#endif  // UNIT_TEST
