/**
 * @file submitted_manifest.cpp
 * @brief Implementation of the pure per-BSSID submitted-to-wpa-sec set (ADR-0065 gate C).
 */
#include "net/submitted_manifest.h"

#include <cstring>

namespace sapper {

bool SubmittedManifest::begin() {
    size_t loaded = 0;
    if (!store_.load(entries_, kMaxSubmittedEntries, loaded)) return false;
    count_ = loaded;
    return true;
}

size_t SubmittedManifest::indexOf(const uint8_t bssid[6]) const {
    for (size_t i = 0; i < count_; ++i) {
        if (std::memcmp(entries_[i].bssid, bssid, 6) == 0) return i;
    }
    return count_;
}

size_t SubmittedManifest::oldestIndex() const {
    // count_ > 0 whenever this is called (only from a full-set eviction).
    size_t oldest = 0;
    for (size_t i = 1; i < count_; ++i) {
        if (entries_[i].uploadedAt < entries_[oldest].uploadedAt) oldest = i;
    }
    return oldest;
}

bool SubmittedManifest::isSubmitted(const uint8_t bssid[6]) const {
    const size_t i = indexOf(bssid);
    return i < count_ && !reverifyDue(entries_[i].lastVerifiedEpoch, nowEpoch_);
}

void SubmittedManifest::markSubmitted(const uint8_t bssid[6], uint32_t nowMs) {
    // Already held → membership is idempotent; keep the original uploadedAt so a long-known submission
    // stays the eviction candidate (it has had the longest chance to be cracked). Mirrors the cracked
    // manifest keeping firstSeenCrackedMs on a re-download (ADR-0065 #3). But DO re-stamp the wall-clock
    // re-verification timer: this call is a terminal re-upload, so it re-verifies the network and
    // re-suppresses it for another interval (ADR-0072 decision 4) — the eviction key and the
    // re-verification key now have distinct roles.
    const size_t existing = indexOf(bssid);
    if (existing < count_) {
        entries_[existing].lastVerifiedEpoch = nowEpoch_;
        return;
    }

    // A BSSID not held — a new submission. Make room at the bound by evicting the oldest (ADR-0065 #1).
    size_t slot = count_;
    if (count_ < kMaxSubmittedEntries) {
        ++count_;
    } else {
        slot = oldestIndex();  // Overwrite the oldest; count stays at the bound.
    }
    std::memcpy(entries_[slot].bssid, bssid, 6);
    entries_[slot].uploadedAt = nowMs;
    entries_[slot].lastVerifiedEpoch = nowEpoch_;  // verified now; suppressed for a full interval (ADR-0072).
}

bool SubmittedManifest::flushSubmitted() {
    return store_.save(entries_, count_);
}

}  // namespace sapper
