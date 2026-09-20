/**
 * @file cracked_manifest.cpp
 * @brief Implementation of the pure per-BSSID cracked-results manifest (ADR-0019 decisions #4, #5).
 */
#include "net/cracked_manifest.h"

#include <cstring>

namespace sapper {

bool CrackedManifest::begin() {
    size_t loaded = 0;
    bool fresh = true;
    if (!store_.load(entries_, kMaxCrackedEntries, loaded, fresh)) return false;
    count_ = loaded;
    fresh_ = fresh;
    return true;
}

size_t CrackedManifest::indexOf(const uint8_t bssid[6]) const {
    for (size_t i = 0; i < count_; ++i) {
        if (std::memcmp(entries_[i].result.bssid, bssid, 6) == 0) return i;
    }
    return count_;
}

size_t CrackedManifest::oldestIndex() const {
    // count_ > 0 whenever this is called (only from a full-manifest eviction).
    size_t oldest = 0;
    for (size_t i = 1; i < count_; ++i) {
        if (entries_[i].firstSeenCrackedMs < entries_[oldest].firstSeenCrackedMs) oldest = i;
    }
    return oldest;
}

const CrackedEntry* CrackedManifest::find(const uint8_t bssid[6]) const {
    const size_t i = indexOf(bssid);
    return i < count_ ? &entries_[i] : nullptr;
}

const CrackedEntry* CrackedManifest::entryAt(size_t i) const {
    return i < count_ ? &entries_[i] : nullptr;
}

ApplyOutcome CrackedManifest::apply(const CrackedResult& result, uint32_t nowMs) {
    const size_t existing = indexOf(result.bssid);
    if (existing < count_) {
        if (std::strcmp(entries_[existing].result.password, result.password) == 0) {
            return ApplyOutcome::Unchanged;  // Same AP, same password — a re-download of a known crack.
        }
        // Rotated PSK: update the recovered secret (and essid) in place, keep the original first-seen.
        entries_[existing].result = result;
        return ApplyOutcome::Changed;
    }

    // A BSSID not held — a new crack. Make room at the bound by evicting the oldest (decision #4).
    size_t slot = count_;
    if (count_ < kMaxCrackedEntries) {
        ++count_;
    } else {
        slot = oldestIndex();  // Overwrite the oldest; count stays at the bound.
    }
    entries_[slot].result = result;
    entries_[slot].firstSeenCrackedMs = nowMs;
    return ApplyOutcome::New;
}

bool CrackedManifest::flush() {
    return store_.save(entries_, count_, fresh_);
}

}  // namespace sapper
