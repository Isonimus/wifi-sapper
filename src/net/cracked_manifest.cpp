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
            // Same AP, same password — a re-download of a known crack. Deliberately does NOT re-stamp
            // lastVerifiedEpoch: the hourly sync re-downloads every known crack, so a bump here would reset
            // the re-verification timer every hour and nothing would ever go stale (ADR-0072 decision 4).
            return ApplyOutcome::Unchanged;
        }
        // Rotated PSK: update the recovered secret (and essid) in place, keep the original first-seen. A
        // real re-crack, so stamp the re-verification timer fresh — this network is now verified again and
        // suppressed for another interval (ADR-0072 decision 4).
        entries_[existing].result = result;
        entries_[existing].lastVerifiedEpoch = nowEpoch_;
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
    // Stamp the re-verification timer from the cached wall-clock (ADR-0072): a freshly cracked (or
    // post-wipe re-seeded) entry is verified now, so it is suppressed for a full interval — no re-seed
    // storm. A pre-NTP nowEpoch_ (< kMinRealEpoch) stamps ~0, treated as due once a real clock exists.
    entries_[slot].lastVerifiedEpoch = nowEpoch_;
    return ApplyOutcome::New;
}

bool CrackedManifest::flush() {
    return store_.save(entries_, count_, fresh_);
}

}  // namespace sapper
