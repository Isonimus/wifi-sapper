/**
 * @file fake_cracked_store.h
 * @brief In-RAM CrackedStore double for the host lane (ADR-0019 decision #4; slice-0020).
 *
 * Backs the CrackedManifest seam the way FakeCaptureStore backs the CaptureStore seam: it holds the
 * persisted entries and the freshManifest flag in memory and round-trips them, so a test proves the
 * mirror/bound/eviction/fresh policy against a faithful stand-in for LittleFS, no filesystem. Failure
 * knobs drive the fail-loud paths (quality bar §3), and a seed helper sets up a non-fresh, pre-populated
 * manifest for the steady-state alerting scenarios.
 */
#pragma once

#include <cstring>
#include <vector>

#include "net/cracked_store.h"

namespace sapper_test {

class FakeCrackedStore : public sapper::CrackedStore {
public:
    bool load(sapper::CrackedEntry* out, size_t capacity, size_t& count, bool& freshManifest) override {
        if (failLoad) return false;
        if (entries_.size() > capacity) return false;  // overrun is a fault to surface, not truncate.
        count = entries_.size();
        for (size_t i = 0; i < entries_.size(); ++i) out[i] = entries_[i];
        freshManifest = fresh_;
        return true;
    }

    bool save(const sapper::CrackedEntry* entries, size_t count, bool freshManifest) override {
        if (failSave) return false;  // prior contents left intact (fail loud — nothing half-written).
        entries_.assign(entries, entries + count);
        fresh_ = freshManifest;
        ++saveCount;
        return true;
    }

    // --- Test setup and observation. ---
    /// Seed a persisted, non-fresh manifest (a device that has already synced at least once).
    void seed(const std::vector<sapper::CrackedEntry>& entries) {
        entries_ = entries;
        fresh_ = false;
    }
    size_t size() const { return entries_.size(); }
    bool fresh() const { return fresh_; }
    const std::vector<sapper::CrackedEntry>& entries() const { return entries_; }

    bool failLoad = false;
    bool failSave = false;
    int saveCount = 0;  ///< How many times save() succeeded — asserts a whole sync is one write.

private:
    std::vector<sapper::CrackedEntry> entries_;
    bool fresh_ = true;  ///< A never-persisted store is fresh (first-boot).
};

}  // namespace sapper_test
