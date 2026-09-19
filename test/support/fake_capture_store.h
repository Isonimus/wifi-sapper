/**
 * @file fake_capture_store.h
 * @brief In-RAM CaptureStore double for the host lane (ADR-0017 decision #3; slice-0018).
 *
 * Backs the CaptureQueue seam the way FakeRadioSniffer backs the RadioSniffer seam: it honours the
 * two contracts the pure policy depends on — monotonically increasing ids that are never reissued,
 * and listPending() ordered oldest-first (ascending id) — so a test proves the bound/dedup/eviction
 * policy against a faithful stand-in for LittleFS, no filesystem. Failure knobs let a test drive the
 * fail-loud paths (quality bar §3) without a real disk fault.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "net/capture_store.h"

namespace sapper_test {

class FakeCaptureStore : public sapper::CaptureStore {
public:
    struct Entry {
        uint32_t id;
        uint8_t bssid[6];
        std::vector<uint8_t> bytes;
    };

    bool enqueue(const uint8_t bssid[6], const uint8_t* pcap, size_t len, uint32_t& outId) override {
        if (failEnqueue) return false;
        Entry entry;
        entry.id = nextId_++;
        std::memcpy(entry.bssid, bssid, 6);
        entry.bytes.assign(pcap, pcap + len);
        outId = entry.id;
        entries_.push_back(std::move(entry));  // push_back keeps insertion (ascending-id) order.
        return true;
    }

    bool listPending(sapper::PendingCapture* out, size_t capacity, size_t& count) override {
        if (failList) return false;
        if (entries_.size() > capacity) return false;  // overrun is a fault to surface, not truncate.
        count = entries_.size();
        for (size_t i = 0; i < entries_.size(); ++i) {
            out[i].id = entries_[i].id;
            std::memcpy(out[i].bssid, entries_[i].bssid, 6);
        }
        return true;
    }

    bool read(uint32_t id, uint8_t* out, size_t capacity, size_t& len) override {
        if (id == failReadId) return false;  // simulate a corrupt/unreadable entry (purge path, #1).
        for (const Entry& entry : entries_) {
            if (entry.id != id) continue;
            if (entry.bytes.size() > capacity) return false;
            len = entry.bytes.size();
            std::memcpy(out, entry.bytes.data(), len);
            return true;
        }
        return false;  // absent.
    }

    bool remove(uint32_t id) override {
        if (failRemove) return false;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].id == id) {
                entries_.erase(entries_.begin() + i);
                return true;
            }
        }
        return false;  // absent.
    }

    // --- Test observation and knobs. ---
    size_t size() const { return entries_.size(); }
    const std::vector<Entry>& entries() const { return entries_; }
    /// Count of pending entries for @p bssid — a test asserts the per-BSSID-dedup invariant with it.
    size_t countForBssid(const uint8_t bssid[6]) const {
        size_t n = 0;
        for (const Entry& entry : entries_) {
            if (std::memcmp(entry.bssid, bssid, 6) == 0) ++n;
        }
        return n;
    }

    bool failEnqueue = false;
    bool failList = false;
    bool failRemove = false;
    uint32_t failReadId = 0;  ///< id whose read() fails (0 = none; ids start at 1) — the purge path.

private:
    std::vector<Entry> entries_;
    uint32_t nextId_ = 1;  // ids start at 1 so 0 can stay a "none" sentinel in callers.
};

}  // namespace sapper_test
