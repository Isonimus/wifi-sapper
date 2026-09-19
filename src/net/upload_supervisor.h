/**
 * @file upload_supervisor.h
 * @brief The batched-drain arbiter above the HuntEngine and the uploader (ADR-0017 decision #5).
 *
 * The component that closes the capture→upload gap ADR-0015 decision #8 left open. It subscribes to
 * the HuntEngine's capture-ready event as a CaptureReadyObserver and does nothing there but enqueue
 * the handshake through the CaptureQueue — cheap, no radio change, the hunt keeps running. Only when
 * enough has accumulated (a pending-count ceiling) or enough time has passed (a time ceiling) does
 * tick() perform one batched drain cycle: stop the engine, wait the quiesce settle (ADR-0015
 * decision #4) so no promiscuous callback is in flight, bring the station up, upload the whole queue
 * over TLS, tear the station down, and resume hunting. Batching amortizes the multi-second STA
 * associate over the whole queue instead of thrashing the radio once per capture (decision #5).
 *
 * Everything here is pure and clock-driven: the engine, queue, uploader, and station control are all
 * injected seams, and time enters only through tick(now), so the whole arbitration — enqueue,
 * threshold, settle, drain, and the bounded-exponential backoff on failure (decision #7) — is
 * host-tested against fakes and a fake clock (ADR-0004 lane 1; slice-0018 Scenarios A-F). The radio,
 * TLS, and wpa-sec bytes are proved only by the on-air verify (Scenario G).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/capture_queue.h"
#include "net/hunt_engine.h"
#include "net/station_control.h"
#include "net/uploader.h"

namespace sapper {

class SyncSession;  // net/sync_session.h — held only as a pointer, so the heavy sync stack it pulls in
                    // (fetcher, manifest, parser) stays out of every translation unit that only uploads.
class EventBus;     // core/event_bus.h — held as a pointer; the supervisor publishes DrainStarted/
                    // DrainCompleted facts for surfaces (ADR-0021), no include needed in this header.
class WindowNotifier;  // net/window_notifier.h — an optional window-sharer flushed inside each STA
                       // window (the push webhook transmits there); held by pointer, wired post-ctor.

/// Forwards the engine's capture-ready event to a target set after construction. It exists to break
/// a construction cycle: the HuntEngine takes its CaptureReadyObserver at construction, but the
/// UploadSupervisor takes the engine at construction, so neither can be built first. The relay is
/// built first, handed to the engine, then aimed at the supervisor — one indirection, no cycle. An
/// event arriving before the target is set is dropped (there is no supervisor to enqueue it yet).
class CaptureReadyRelay : public CaptureReadyObserver {
public:
    void setTarget(CaptureReadyObserver& target) { target_ = &target; }
    void onCaptureReady(const CapturedHandshake& handshake) override {
        if (target_ != nullptr) target_->onCaptureReady(handshake);
    }

private:
    CaptureReadyObserver* target_ = nullptr;
};

/// Drain arbitration tuning. Defaults are architecture-derived; settleMs must track the engine's own
/// HuntConfig.settleMs (ADR-0015 decision #4) since it guards the same in-flight-callback hazard.
struct UploadSupervisorConfig {
    size_t drainThreshold = 8;             ///< Pending captures that trigger a drain on their own.
    uint32_t maxDrainIntervalMs = 300000;  ///< Drain at least this often while anything is pending.
    uint32_t settleMs = 20;                ///< Quiesce settle after stop() before the STA mode change.
    uint32_t backoffBaseMs = 30000;        ///< Delay before the first retry after a failed cycle.
    uint32_t backoffCapMs = 900000;        ///< Ceiling the backoff grows to; must be >= backoffBaseMs,
                                           ///< or the first delay exceeds the "cap" before it clamps.
    uint32_t syncRetryIntervalMs = 60000;  ///< Min gap between sync-only STA windows (ADR-0019 decision
                                           ///< #6). A due sync opens its own window on this cadence — not
                                           ///< every tick (no radio spin) and NOT coupled to
                                           ///< maxDrainIntervalMs, so raising the upload ceiling cannot
                                           ///< starve the hourly sync. A *successful* sync stops being due
                                           ///< (SyncSession), so this only paces retries of a failing one.
};

/// The observable result of the most recent drain cycle. The supervisor uploads but does not sync or
/// alert (ADR-0017 decision #6); this getter is how the device surface reports a cycle and how the
/// on-air verify reads it, and it is the seam slice-6/7 will turn into a real event subscription.
struct DrainOutcome {
    bool ran = false;          ///< A cycle executed (as opposed to none yet).
    bool associated = false;   ///< The station came up TLS-ready.
    size_t accepted = 0;       ///< Uploads wpa-sec took fresh.
    size_t duplicate = 0;      ///< Uploads wpa-sec already held (also terminal success).
    size_t rejected = 0;       ///< Uploads kept for retry (non-success or unrecognised — decision #2).
    size_t purged = 0;         ///< Unreadable (corrupt) captures dropped so the queue can go clean (#1).
    size_t storeErrors = 0;    ///< Removes that could not complete — a purge or post-upload delete that
                               ///< failed, leaving the entry stuck (fail loud — §3; keeps the cycle dirty).
    bool resumeFailed = false; ///< The engine could not re-enter promiscuous mode after the drain.
};

/**
 * @brief Arbitrates batched wpa-sec uploads against the endless hunt.
 *
 * Construct with the engine it pauses, the queue it drains, the uploader and station-control seams,
 * the wpa-sec key, and the tuning. Wire it as the engine's CaptureReadyObserver, call begin(now)
 * once the engine is running, then tick(now) every app-loop iteration.
 */
class UploadSupervisor : public CaptureReadyObserver {
public:
    /// @param sync Optional cracked-results sync to share each STA window with (ADR-0019 decision #6).
    ///        nullptr on a build with no sync (or in an upload-only test): the supervisor then behaves
    ///        exactly as slice-0018 shipped it.
    /// @param bus Optional surface event bus (ADR-0021). When set, the supervisor publishes DrainStarted
    ///        as it opens a window and DrainCompleted after each cycle, for surfaces (the LED) to react
    ///        to. nullptr in an upload-only test leaves the drain behaviour untouched.
    UploadSupervisor(HuntEngine& engine, CaptureQueue& queue, Uploader& uploader,
                     StationControl& station, const char* wpaSecKey,
                     const UploadSupervisorConfig& config = {}, SyncSession* sync = nullptr,
                     EventBus* bus = nullptr);

    /// Wire an optional window-sharer flushed inside every STA window (ADR-0023): a transmitting
    /// surface (the push webhook) that needs connectivity. Set post-construction, mirroring the
    /// CaptureReadyRelay wiring, so the constructor does not grow a ninth argument. Unset → no flush,
    /// and the drain behaves exactly as slice-0018/0022 shipped it.
    void setNotifier(WindowNotifier& notifier) { notifier_ = &notifier; }

    /// Seed the drain clock and pick up any captures already on flash from a prior run. Call after
    /// the engine has begun and before the first tick().
    void begin(uint32_t nowMs);

    /// Capture-ready seam (app task): serialize and enqueue the handshake. Never changes the radio.
    void onCaptureReady(const CapturedHandshake& handshake) override;

    /// Advance the drain state machine: decide whether to drain, run the settle, and run one cycle.
    void tick(uint32_t nowMs);

    const DrainOutcome& lastDrain() const { return lastDrain_; }
    size_t pendingActivity() const { return activity_; }
    /// Monotonic count of drain cycles run — lets an observer detect each new drain (the outcomes
    /// themselves can be identical), so a device surface / verify can report every cycle, not just one.
    uint32_t drainCount() const { return drainCount_; }

private:
    enum class State { Hunting, Settling };

    /// Whether to open an STA window now: an upload drain is warranted (threshold or time ceiling, past
    /// the backoff) or a cracked-results sync has come due (ADR-0019 decision #6). A sync opens a window
    /// on its own only when nothing else would, rate-limited by the same time ceiling so a persistently
    /// failing sync retries on a cadence, not every tick.
    bool shouldOpenWindow(uint32_t nowMs) const;
    void runDrainCycle(uint32_t nowMs);
    /// Upload every pending capture once, recording counts in @p outcome; return whether the cycle
    /// fully succeeded (every upload terminal, no store error).
    bool drainQueue(DrainOutcome& outcome);

    HuntEngine& engine_;
    CaptureQueue& queue_;
    Uploader& uploader_;
    StationControl& station_;
    const char* wpaSecKey_;
    UploadSupervisorConfig config_;
    SyncSession* sync_;  ///< Optional; the sync shares the drain's STA window (ADR-0019 decision #6).
    EventBus* bus_;      ///< Optional; publishes DrainStarted/DrainCompleted facts for surfaces (ADR-0021).
    WindowNotifier* notifier_ = nullptr;  ///< Optional; flushed inside each STA window (ADR-0023).

    State state_ = State::Hunting;
    size_t activity_ = 0;             ///< Uploadable captures known pending since the last success.
    uint32_t settleUntilMs_ = 0;      ///< When the post-stop() settle elapses (Settling → drain).
    uint32_t lastDrainMs_ = 0;        ///< When the last cycle ran, for the time ceiling.
    uint32_t nextDrainAllowedMs_ = 0; ///< Backoff gate: no cycle before this (decision #7).
    uint32_t nextSyncWindowMs_ = 0;   ///< Earliest a sync-only window may open again (ADR-0019 decision #6).
    uint32_t backoffMs_ = 0;          ///< Current backoff delay; grows on failure, resets on success.
    uint32_t drainCount_ = 0;         ///< Monotonic count of cycles run (drainCount()).
    DrainOutcome lastDrain_;
    uint8_t uploadBuffer_[kMaxSerializedPcapLen];  ///< Read target during a drain; no heap.
};

}  // namespace sapper
