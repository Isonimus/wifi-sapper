/**
 * @file sync_session.h
 * @brief The cracked-results sync as the drain supervisor sees it — the seam that lets the hourly sync
 *        share the STA window the upload drain already brings up (ADR-0019 decision #6, ADR-0017 #6).
 *
 * The UploadSupervisor owns the one promiscuous↔STA transition on the radio (ADR-0015; §4 invariant
 * #11). To sync cracked results it must run inside that same window, never open a second concurrent STA
 * path. This seam is how it does that without knowing the scheduler, fetcher, or manifest behind it:
 * begin() seeds the cadence at boot, due() tells the supervisor a sync is owed (so it opens one window
 * even with an empty upload queue), and runInWindow() runs the sync inside a window the supervisor has
 * already associated. Keeping it abstract lets the supervisor's window-sharing logic be host-tested
 * against a trivial fake (test/support/fake_sync_session.h) instead of the whole sync stack, exactly as
 * StationControl and Uploader are (ADR-0004 lane 1).
 *
 * The concrete ScheduledSyncSession binds the pure SyncScheduler to the pure CrackedSync and owns the
 * one cadence rule the supervisor must not have to know: a successful sync advances the hourly deadline,
 * a failed one does not, so a failure retries on the next window rather than waiting a full hour
 * (ADR-0019 decision #7). The supervisor therefore does not track sync success at all — the sync's
 * outcome is independent of the upload backoff (a download outage must not drag out upload cadence).
 */
#pragma once

#include <cstdint>

#include "net/cracked_sync.h"
#include "net/sync_scheduler.h"

namespace sapper {

/// The sync as the supervisor drives it: seed at boot, ask whether one is due, run one inside a live
/// STA window. Host-faked for the window-sharing tests; device-bound by ScheduledSyncSession.
class SyncSession {
public:
    virtual ~SyncSession() = default;

    /// Seed the cadence clock at boot so the first sync becomes due on an early STA window, not an hour
    /// in (called from UploadSupervisor::begin).
    virtual void begin(uint32_t nowMs) = 0;

    /// Is a sync owed as of @p nowMs? True lets the supervisor open one window on the sync's behalf even
    /// when the upload queue is empty (ADR-0019 decision #6).
    virtual bool due(uint32_t nowMs) const = 0;

    /// Run one sync inside the STA window the supervisor has already brought up. The session advances its
    /// own cadence on success and leaves it due on failure (decision #7); the supervisor neither needs
    /// nor reads the result, so this cannot couple a sync failure to the upload backoff.
    virtual void runInWindow(uint32_t nowMs) = 0;
};

/// The device binding: a pure SyncScheduler for the *when* and a pure CrackedSync for the *what*. The
/// only logic beyond delegation is the decision-#7 cadence rule — noteSynced() only on a successful
/// sync — kept here so both the supervisor and the scheduler stay ignorant of each other.
class ScheduledSyncSession : public SyncSession {
public:
    ScheduledSyncSession(SyncScheduler& scheduler, CrackedSync& sync)
        : scheduler_(scheduler), sync_(sync) {}

    void begin(uint32_t nowMs) override { scheduler_.begin(nowMs); }

    bool due(uint32_t nowMs) const override { return scheduler_.isDue(nowMs); }

    void runInWindow(uint32_t nowMs) override {
        const SyncOutcome outcome = sync_.runSync(nowMs);
        // Only a successful sync pushes the hourly deadline out; a failure leaves it due so the next STA
        // window retries rather than waiting a full hour (ADR-0019 decision #7; slice-0020 Scenario I).
        if (outcome.ok) scheduler_.noteSynced(nowMs);
    }

private:
    SyncScheduler& scheduler_;
    CrackedSync& sync_;
};

}  // namespace sapper
