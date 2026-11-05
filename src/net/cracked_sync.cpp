/**
 * @file cracked_sync.cpp
 * @brief Implementation of the pure cracked-results sync orchestration (ADR-0019 decisions #3, #5, #7).
 */
#include "net/cracked_sync.h"

namespace sapper {

void CrackedSync::onLine(std::string_view line) {
    CrackedResult result;
    // Each source carries a different field layout, so the active source selects its sibling parser; the
    // CrackedResult it yields is backend-agnostic, so the shared manifest stays unaware of the source
    // (ADR-0109 #5).
    const ParseLineResult parsed = (activeSource_ == Source::Pwncrack)
                                       ? parsePwncrackPotfileLine(line, result)
                                       : parseCrackedLine(line, result);
    switch (parsed) {
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

FetchResult CrackedSync::fetchSource(CrackedResultsFetcher& fetcher, const char* key, Source src,
                                     bool& allUsable) {
    const size_t mark = bufferCount_;
    const size_t malformedBefore = malformed_;
    activeSource_ = src;
    const FetchResult result = fetcher.fetch(key, *this);
    const size_t added = bufferCount_ - mark;
    const size_t malformedAdded = malformed_ - malformedBefore;

    // A source is usable only if it fully transferred (Ok) AND did not yield a body of nothing-but-
    // malformed lines (a garbage/truncated body that a chunk boundary or a format change produced). An
    // unusable source contributes nothing: roll its partial lines back off the shared buffer so the
    // manifest is only ever applied from sources that delivered a usable body (Scenario I, per-source).
    // It does NOT fail the whole sync: a sibling source's good results still apply, so a flaky second
    // backend never blocks the primary's cracks (ADR-0109 #5), but it clears allUsable, so the sync is
    // not marked ok (the scheduler retries it next window) and the first-sync baseline is not completed.
    const bool usable = (result == FetchResult::Ok) && !(added == 0 && malformedAdded > 0);
    if (!usable) {
        // Roll back only the buffered RESULTS (bufferCount_), never malformed_/overflow_. Those two are
        // the cross-source parse-drift diagnostic the outcome reports ("a non-zero count is a format-change
        // signal"), and an all-malformed source is exactly when that signal matters most: a reader must not
        // "symmetrise" this by resetting malformed_/overflow_ to their marks, which would hide a pwncrack
        // layout change behind a discarded body (quality bar §3 fail-loud; the counts drive no alert/apply,
        // only the SyncCompleted log, so they never mis-gate the manifest).
        bufferCount_ = mark;
        allUsable = false;
    }
    return result;
}

SyncOutcome CrackedSync::runSync(uint32_t nowMs) {
    bufferCount_ = 0;
    malformed_ = 0;
    overflow_ = 0;

    SyncOutcome outcome;
    bool allUsable = true;

    // Fetch every provisioned source into the one buffer (wpa-sec always, pwncrack when wired), each with
    // its own parser and per-source rollback. Both feed the same manifest; the fresh/seed and alert
    // policy below is per-manifest, not per-source (ADR-0109 #5).
    outcome.fetch = fetchSource(fetcher_, wpaSecKey_, Source::WpaSec, allUsable);
    if (pwncrackProvisioned()) {
        outcome.pwncrackRan = true;
        outcome.pwncrackFetch = fetchSource(*pwncrackFetcher_, pwncrackKey_, Source::Pwncrack, allUsable);
    }
    outcome.downloaded = bufferCount_;
    outcome.malformed = malformed_;
    outcome.overflow = overflow_;

    const bool wasFresh = manifest_.isFresh();
    outcome.firstSync = wasFresh;

    // Apply every surviving result (unusable sources already rolled themselves back, so nothing here came
    // from a failed/garbage source). In steady state note which are New/Changed to announce after flush.
    size_t alertCount = 0;
    for (size_t i = 0; i < bufferCount_; ++i) {
        const ApplyOutcome applied = manifest_.apply(buffer_[i], nowMs);
        if (!wasFresh && (applied == ApplyOutcome::New || applied == ApplyOutcome::Changed)) {
            alertIdx_[alertCount++] = static_cast<uint16_t>(i);
        }
    }

    // Clear fresh only once the baseline is complete: every provisioned source succeeded this sync. A
    // partial first sync seeds what it got silently and stays fresh, so the next full sync still seeds
    // the rest silently rather than alerting a backlog from the source that was down (ADR-0109 #5: the
    // first-sync-silent-seed stays per-manifest, not per-source).
    const bool clearingFresh = wasFresh && allUsable;

    // Persist only when the sync changed persisted state: a result was applied, or the first-sync
    // baseline is being completed (the fresh flag flips). A pure failure (nothing applied, baseline not
    // completing) leaves the store exactly as it was: no write, manifest untouched (Scenario I).
    if (bufferCount_ > 0 || clearingFresh) {
        if (clearingFresh) manifest_.clearFresh();
        // Persist before announcing: never announce a crack that was not durably recorded (quality bar §3).
        if (!manifest_.flush()) {
            outcome.storeError = true;
            bus_.publish(AppEvent::syncCompleted(outcome));
            return outcome;
        }
    }

    if (clearingFresh) {
        bus_.publish(AppEvent::firstSyncSummary(bufferCount_));  // One summary for the seeded backlog (decision #5).
    } else if (!wasFresh) {
        for (size_t i = 0; i < alertCount; ++i) bus_.publish(AppEvent::newPassword(buffer_[alertIdx_[i]]));
        outcome.newPasswords = alertCount;
    }
    // A partial first sync (wasFresh and not clearingFresh) seeds silently with no summary: the baseline
    // is not yet established, so a later full sync will summarise it.

    outcome.ok = allUsable;
    bus_.publish(AppEvent::syncCompleted(outcome));
    return outcome;
}

}  // namespace sapper
