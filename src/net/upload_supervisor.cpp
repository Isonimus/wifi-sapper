/**
 * @file upload_supervisor.cpp
 * @brief Implementation of the batched-drain upload arbiter (ADR-0017).
 */
#include "net/upload_supervisor.h"

#include <cstring>  // memcpy/strncpy: copy the capture identity into the bus payload.

#include "core/deadline.h"  // reached(): the wrap-safe deadline test shared across clock-driven units.
#include "core/event_bus.h"
#include "net/sync_session.h"
#include "net/window_notifier.h"

namespace sapper {

UploadSupervisor::UploadSupervisor(HuntEngine& engine, CaptureQueue& queue, Uploader& uploader,
                                   StationControl& station, const char* wpaSecKey,
                                   const UploadSupervisorConfig& config, SyncSession* sync,
                                   EventBus* bus)
    : engine_(engine),
      queue_(queue),
      uploader_(uploader),
      station_(station),
      wpaSecKey_(wpaSecKey),
      config_(config),
      sync_(sync),
      bus_(bus) {}

void UploadSupervisor::begin(uint32_t nowMs) {
    lastDrainMs_ = nowMs;
    nextDrainAllowedMs_ = nowMs;
    nextSyncWindowMs_ = nowMs;  // a due sync may open its first window at once, not an upload-ceiling later.
    backoffMs_ = config_.backoffBaseMs;
    state_ = State::Hunting;
    if (sync_ != nullptr) sync_->begin(nowMs);  // seed the hourly cadence so an early window catches up.

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
            // The data has landed in the queue (§4 #13's must-deliver seam did its job); now broadcast
            // the cosmetic "it happened" fact so the local surfaces can announce it (ADR-0031). Publish
            // *after* the enqueue, and only on a success arm, so the fact never claims a capture the
            // queue dropped. The payload is identity-only — bssid + ssid, never the pcap frames (§4 #9/
            // #17) — so no surface can reach capture bytes through the bus.
            publishCaptured(handshake);
            return;
        case OfferResult::NotUploadable:
        case OfferResult::StoreError:
            return;  // nothing newly pending to drain; the failure (if any) is the queue's to report.
    }
}

void UploadSupervisor::publishCaptured(const CapturedHandshake& handshake) {
    if (bus_ == nullptr) return;
    CaptureFact fact;  // stack-local, held live across the synchronous publish() (ADR-0021).
    std::memcpy(fact.bssid, handshake.bssid, sizeof(fact.bssid));
    std::strncpy(fact.ssid, handshake.ssid, sizeof(fact.ssid) - 1);
    fact.ssid[sizeof(fact.ssid) - 1] = '\0';  // strncpy does not NUL-terminate a full-length source.
    bus_->publish(AppEvent::handshakeCaptured(fact));
}

bool UploadSupervisor::shouldOpenWindow(uint32_t nowMs) const {
    if (!reached(nowMs, nextDrainAllowedMs_)) return false;  // inside a backoff — no associate at all.
    if (activity_ >= config_.drainThreshold) return true;    // enough queued to amortize an associate.
    const bool timeCeilingReached = reached(nowMs, lastDrainMs_ + config_.maxDrainIntervalMs);
    if (activity_ > 0 && timeCeilingReached) return true;    // else drain the trickle on the time ceiling.
    // A due cracked-results sync opens one window on its own when nothing else would (ADR-0019 decision
    // #6: the hour comes due while the upload queue stays empty). It is gated by its OWN retry cadence
    // (nextSyncWindowMs_, advanced after each sync), not the upload time ceiling: a persistently-failing
    // sync retries on syncRetryIntervalMs rather than every tick (decision #7 wants "the next window", not
    // a spin), and raising maxDrainIntervalMs for a low-activity deployment cannot starve the hourly sync.
    // When a drain opens a window for its own reason, runDrainCycle piggybacks a due sync into it for free,
    // so this clause only handles the sync-alone case.
    return sync_ != nullptr && sync_->due(nowMs) && reached(nowMs, nextSyncWindowMs_);
}

void UploadSupervisor::tick(uint32_t nowMs) {
    switch (state_) {
        case State::Hunting:
            if (!shouldOpenWindow(nowMs)) return;
            // Stop the engine (atomic router → nullptr, sniffer stopped — §4 invariant #11), then let
            // the settle elapse before touching the radio mode, so no promiscuous callback is still in
            // flight when the STA associate changes Wi-Fi state (ADR-0015 decision #4 / decision #5b).
            engine_.stop();
            state_ = State::Settling;
            settleUntilMs_ = nowMs + config_.settleMs;
            // The appliance is now going off-air to do network work; tell surfaces (ADR-0021). This
            // fires for a sync-only window too — the LED shows "working" for any STA window, not only
            // an upload drain.
            if (bus_ != nullptr) bus_->publish(AppEvent::drainStarted());
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
        cycleSucceeded = drainQueue(outcome);   // no-op success when the queue is empty (a sync-only window).
        // Share this live STA window with a due cracked-results sync before teardown (ADR-0019 decision
        // #6). The session owns its own cadence and its outcome is deliberately NOT folded into
        // cycleSucceeded: a wpa-sec download outage must not drag the upload backoff out — a failed sync
        // simply stays due and retries on the next window (decision #7).
        if (sync_ != nullptr && sync_->due(nowMs)) {
            sync_->runInWindow(nowMs);
            // Pace the next sync-only window from here: a sync that failed stays due, and this gap keeps it
            // off the radio every tick; a sync that succeeded is no longer due, so the gate is moot.
            nextSyncWindowMs_ = nowMs + config_.syncRetryIntervalMs;
        }
        // Give a transmitting surface (the push webhook) this live window before tear-down (ADR-0023):
        // a password cracked by the sync moments ago is pushed the same window, and any send that
        // failed a previous window retries now. After the sync, so this window's fresh cracks are
        // already enqueued; before tear-down, so the station is still associated.
        if (notifier_ != nullptr) notifier_->flushInWindow();
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
        // A failed upload drain leaves activity_ as it was (it is only ever reset to 0 on success), so the
        // trigger stays hot on its own — no need to fake activity here. A failed *sync-only* window has
        // activity_ == 0 and must stay that way: faking an upload would open a needless empty drain after
        // the sync eventually succeeds; the sync's own due() drives its retry (gated by the ceiling above).
    }
    lastDrain_ = outcome;
    ++drainCount_;
    // Publish the counted result for surfaces (ADR-0021). Same struct the lastDrain() snapshot exposes
    // to the on-air verify — one computation, pushed to surfaces and retained for the harness.
    if (bus_ != nullptr) bus_->publish(AppEvent::drainCompleted(lastDrain_));
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
