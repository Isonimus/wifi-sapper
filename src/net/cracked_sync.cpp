/**
 * @file cracked_sync.cpp
 * @brief Implementation of the pure cracked-results sync orchestration (ADR-0019 decisions #3, #5, #7).
 */
#include "net/cracked_sync.h"

namespace sapper {

void CrackedSync::onLine(std::string_view line) {
    CrackedResult result;
    switch (parseCrackedLine(line, result)) {
        case ParseLineResult::Ok:
            if (bufferCount_ < kMaxCrackedEntries) {
                buffer_[bufferCount_++] = result;
            } else {
                ++overflow_;  // Account exceeds the mirror bound — the ledgered churn case (decision #4).
            }
            break;
        case ParseLineResult::Malformed:
            ++malformed_;  // Skipped and counted so a format change is loud (decision #3).
            break;
        case ParseLineResult::Empty:
            break;  // Blank line — not a result and not a fault.
    }
}

SyncOutcome CrackedSync::runSync(uint32_t nowMs) {
    bufferCount_ = 0;
    malformed_ = 0;
    overflow_ = 0;

    SyncOutcome outcome;
    outcome.fetch = fetcher_.fetch(wpaSecKey_, *this);
    outcome.downloaded = bufferCount_;
    outcome.malformed = malformed_;
    outcome.overflow = overflow_;

    // A transport/auth failure, or a body that yielded no usable line but did yield malformed ones
    // (garbage/truncated), is a failed sync: leave the manifest exactly as it was and announce nothing
    // (ADR-0019 decision #7; Scenario I). An empty account (no results, no malformed) is a success.
    if (outcome.fetch != FetchResult::Ok || (bufferCount_ == 0 && malformed_ > 0)) {
        observer_.onSyncOutcome(outcome);
        return outcome;
    }

    const bool wasFresh = manifest_.isFresh();
    outcome.firstSync = wasFresh;

    // Apply every buffered result; in steady state, note which are New/Changed to announce after flush.
    size_t alertCount = 0;
    for (size_t i = 0; i < bufferCount_; ++i) {
        const ApplyOutcome applied = manifest_.apply(buffer_[i], nowMs);
        if (!wasFresh && (applied == ApplyOutcome::New || applied == ApplyOutcome::Changed)) {
            alertIdx_[alertCount++] = static_cast<uint16_t>(i);
        }
    }
    if (wasFresh) manifest_.clearFresh();

    // Persist before announcing: never announce a crack that was not durably recorded (quality bar §3).
    if (!manifest_.flush()) {
        outcome.storeError = true;
        observer_.onSyncOutcome(outcome);
        return outcome;
    }

    if (wasFresh) {
        observer_.onFirstSyncSummary(bufferCount_);  // One summary for the seeded backlog (decision #5).
    } else {
        for (size_t i = 0; i < alertCount; ++i) observer_.onNewPassword(buffer_[alertIdx_[i]]);
        outcome.newPasswords = alertCount;
    }

    outcome.ok = true;
    observer_.onSyncOutcome(outcome);
    return outcome;
}

}  // namespace sapper
