/**
 * @file upload_supervisor.cpp
 * @brief Implementation of the batched-drain upload arbiter (ADR-0017).
 */
#include "net/upload_supervisor.h"

namespace sapper {
namespace {

/// Wrap-safe "has @p nowMs reached @p deadlineMs?" — the same discipline HuntEngine uses, because
/// this appliance runs across the ~49.7-day millis() wrap as normal operation and a direct
/// `now >= deadline` would fire a drain (or collapse the settle) at the wrap. Correct while the true
/// interval stays under 2^31 ms; every drain/backoff/settle window is. This is the second use of the
/// absolute-deadline form (HuntEngine has the first); a third caller is the point to extract a shared
/// helper (quality bar §3 rule of three) — two self-contained copies with this comment stay clearer.
bool reached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace

UploadSupervisor::UploadSupervisor(HuntEngine& engine, CaptureQueue& queue, Uploader& uploader,
                                   StationControl& station, const char* wpaSecKey,
                                   const UploadSupervisorConfig& config)
    : engine_(engine),
      queue_(queue),
      uploader_(uploader),
      station_(station),
      wpaSecKey_(wpaSecKey),
      config_(config) {}

void UploadSupervisor::begin(uint32_t nowMs) {
    lastDrainMs_ = nowMs;
    nextDrainAllowedMs_ = nowMs;
    backoffMs_ = config_.backoffBaseMs;
    state_ = State::Hunting;

    // Pick up captures a prior run left on flash so a reboot does not strand them below the count
    // threshold; the time ceiling would eventually drain them, but seeding lets a full queue drain at
    // once. If the listing fails we do NOT know the queue is empty, so keep the trigger warm
    // (activity_ = 1): otherwise shouldDrain()'s activity_==0 short-circuit would suppress even the time
    // ceiling and strand every already-persisted capture until some new one happened to arrive. A warm
    // trigger makes the next tick run a drain, which re-reads the queue and self-corrects.
    PendingCapture pending[kCaptureStoreScanCapacity];
    size_t count = 0;
    activity_ = queue_.pending(pending, kCaptureStoreScanCapacity, count) ? count : 1;
}

void UploadSupervisor::onCaptureReady(const CapturedHandshake& handshake) {
    // The capture-ready seam does exactly one thing: enqueue. No radio change, so the hunt keeps
    // running (ADR-0017 decision #5). The result feeds only the drain trigger; a store failure is
    // surfaced by the queue and reflected in the next cycle's storeErrors, not swallowed here.
    switch (queue_.offer(handshake)) {
        case OfferResult::Stored:
        case OfferResult::Replaced:
        case OfferResult::Evicted:
            // Replaced/Evicted keep the net count the same, but each is fresh capture activity worth
            // draining, so they count toward the trigger just as a plain Stored does.
            ++activity_;
            return;
        case OfferResult::NotUploadable:
        case OfferResult::StoreError:
            return;  // nothing newly pending to drain; the failure (if any) is the queue's to report.
    }
}

bool UploadSupervisor::shouldDrain(uint32_t nowMs) const {
    if (activity_ == 0) return false;                       // nothing to send.
    if (!reached(nowMs, nextDrainAllowedMs_)) return false; // still inside a backoff (decision #7).
    if (activity_ >= config_.drainThreshold) return true;   // enough accumulated to amortize an associate.
    return reached(nowMs, lastDrainMs_ + config_.maxDrainIntervalMs);  // else drain on the time ceiling.
}

void UploadSupervisor::tick(uint32_t nowMs) {
    switch (state_) {
        case State::Hunting:
            if (!shouldDrain(nowMs)) return;
            // Stop the engine (atomic router → nullptr, sniffer stopped — §4 invariant #11), then let
            // the settle elapse before touching the radio mode, so no promiscuous callback is still in
            // flight when the STA associate changes Wi-Fi state (ADR-0015 decision #4 / decision #5b).
            engine_.stop();
            state_ = State::Settling;
            settleUntilMs_ = nowMs + config_.settleMs;
            return;

        case State::Settling:
            if (!reached(nowMs, settleUntilMs_)) return;  // settle not yet elapsed.
            runDrainCycle(nowMs);
            state_ = State::Hunting;
            return;
    }
}

void UploadSupervisor::runDrainCycle(uint32_t nowMs) {
    DrainOutcome outcome;
    outcome.ran = true;

    // Bring the station up (associate + NTP → TLS-ready) and, only if that succeeds, drain. A failed
    // bring-up is an offline/out-of-range cycle: no uploads attempted, back off (decision #7).
    bool cycleSucceeded = false;
    if (station_.bringUpStation()) {
        outcome.associated = true;
        cycleSucceeded = drainQueue(outcome);
        station_.tearDownStation();
    }

    // Resume promiscuous hunting no matter what the drain did — the appliance's job is to keep
    // hunting. A failure to re-enter promiscuous mode is a hard fault the device surface must report.
    if (!engine_.begin(nowMs)) outcome.resumeFailed = true;

    lastDrainMs_ = nowMs;
    if (cycleSucceeded) {
        backoffMs_ = config_.backoffBaseMs;   // reset: the tick after a success may drain again at once.
        nextDrainAllowedMs_ = nowMs;
        activity_ = 0;                         // the queue drained clean; nothing known pending.
    } else {
        nextDrainAllowedMs_ = nowMs + backoffMs_;                          // penalise the failure,
        backoffMs_ = backoffMs_ >= config_.backoffCapMs / 2 ? config_.backoffCapMs
                                                            : backoffMs_ * 2;  // then grow, capped.
        if (activity_ == 0) activity_ = 1;     // keep the trigger hot so the retry fires at backoff end.
    }
    lastDrain_ = outcome;
    ++drainCount_;
}

bool UploadSupervisor::drainQueue(DrainOutcome& outcome) {
    PendingCapture pending[kCaptureStoreScanCapacity];
    size_t count = 0;
    if (!queue_.pending(pending, kCaptureStoreScanCapacity, count)) {
        ++outcome.storeErrors;   // cannot even list the queue — fail loud, treat the cycle as failed.
        return false;
    }

    bool allTerminal = true;
    for (size_t i = 0; i < count; ++i) {
        size_t len = 0;
        if (!queue_.read(pending[i].id, uploadBuffer_, sizeof(uploadBuffer_), len)) {
            // Bytes unreadable/oversized. On a single-threaded LittleFS this is corruption, not a
            // transient, so keeping the entry would never help and would pin the cycle "dirty" forever,
            // wedging backoff at its cap (ADR-0017 decision #7). Purge it: the bytes are already
            // unrecoverable, so this drops no capture we could have sent, and it lets the queue reach a
            // clean state. Loud via outcome.purged. Only a *failed* remove keeps the cycle dirty.
            ++outcome.purged;
            if (!queue_.remove(pending[i].id)) {
                ++outcome.storeErrors;
                allTerminal = false;
            }
            continue;
        }
        switch (uploader_.upload(uploadBuffer_, len, wpaSecKey_)) {
            case UploadResult::Accepted:
                ++outcome.accepted;
                if (!queue_.remove(pending[i].id)) ++outcome.storeErrors;  // uploaded; delete failed.
                break;
            case UploadResult::Duplicate:
                ++outcome.duplicate;  // wpa-sec already holds it — terminal success, delete it too.
                if (!queue_.remove(pending[i].id)) ++outcome.storeErrors;
                break;
            case UploadResult::Rejected:
                ++outcome.rejected;   // non-success/unrecognised: keep and retry, never assume success.
                allTerminal = false;
                break;
        }
    }
    // Success only when every attempted upload was terminal AND no remove failed — an associated cycle
    // that still had a rejection or a stuck remove backs off and retries (decision #7, Scenario C). A
    // purged corrupt entry does NOT block success: it is resolved (gone), so a cycle that only had to
    // purge still counts clean and lets backoff recover. Terminal uploads and purges shrink the queue;
    // the rejected leftovers wait for the next cycle.
    return allTerminal && outcome.storeErrors == 0;
}

}  // namespace sapper
