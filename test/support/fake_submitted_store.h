/**
 * @file fake_submitted_store.h
 * @brief In-RAM SubmittedStore double for the host lane (ADR-0065 gate C).
 *
 * Backs the SubmittedManifest seam the way FakeCrackedStore backs the CrackedStore seam: it holds the
 * persisted entries in memory and round-trips them, so a test proves the set/bound/eviction policy
 * against a faithful stand-in for LittleFS, no filesystem. Failure knobs drive the fail-loud paths
 * (quality bar §3), and a seed helper sets up a pre-populated set for the drop/skip scenarios.
 */
#pragma once

#include <vector>

#include "net/submitted_store.h"

namespace sapper_test {

class FakeSubmittedStore : public sapper::SubmittedStore {
public:
    bool load(sapper::SubmittedEntry* out, size_t capacity, size_t& count) override {
        if (failLoad) return false;
        if (entries_.size() > capacity) return false;  // overrun is a fault to surface, not truncate.
        count = entries_.size();
        for (size_t i = 0; i < entries_.size(); ++i) out[i] = entries_[i];
        return true;
    }

    bool save(const sapper::SubmittedEntry* entries, size_t count) override {
        if (failSave) return false;  // prior contents left intact (fail loud — nothing half-written).
        entries_.assign(entries, entries + count);
        ++saveCount;
        return true;
    }

    // --- Test setup and observation. ---
    /// Seed a persisted set (a device that has already submitted networks in a prior run).
    void seed(const std::vector<sapper::SubmittedEntry>& entries) { entries_ = entries; }
    size_t size() const { return entries_.size(); }
    const std::vector<sapper::SubmittedEntry>& entries() const { return entries_; }

    bool failLoad = false;
    bool failSave = false;
    int saveCount = 0;  ///< How many times save() succeeded — asserts a whole drain is one write.

private:
    std::vector<sapper::SubmittedEntry> entries_;
};

}  // namespace sapper_test
