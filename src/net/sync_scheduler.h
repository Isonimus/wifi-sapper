/**
 * @file sync_scheduler.h
 * @brief The pure hourly cadence for the cracked-results sync (ADR-0019 decision #6).
 *
 * A clock-driven, wrap-safe timer that answers one question: is a sync due? It owns no radio and no
 * network — the UploadSupervisor asks isDue() as it runs a drain and, per ADR-0019 decision #6, either
 * piggybacks a due sync on the STA window a drain already brings up or (when the upload queue is empty
 * and the hour has come) forces one window for the sync alone. Keeping the *when* here, pure and
 * fake-clock host-tested, keeps that decision out of the device timing entirely (ADR-0004 lane 1;
 * slice-0020 Scenario H).
 *
 * Wrap safety uses the same signed-difference deadline as every other window in the engine
 * (static_cast<int32_t>(nowMs - dueAtMs) >= 0), so a millis() rollover on a long-running appliance
 * never makes a sync perpetually due or perpetually not (ADR-0015 wrap-safe deadline pattern).
 *
 * A successful sync is the only thing that pushes the next deadline out (noteSynced); a *failed* sync
 * does not, so the next STA window retries rather than waiting a full hour (ADR-0019 decision #7). The
 * first sync is due at begin() so a just-booted appliance catches up its account on the first window,
 * not an hour later.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// The cracked-results sync cadence: once an hour (ADR-0019 decision #6; the appliance's stated loop).
constexpr uint32_t kSyncIntervalMs = 3600000;

/// Tracks when the next cracked-results sync is due. Construct (optionally with a shorter interval for
/// a test), begin() at boot, isDue() to check, noteSynced() after each *successful* sync.
class SyncScheduler {
public:
    explicit SyncScheduler(uint32_t intervalMs = kSyncIntervalMs) : intervalMs_(intervalMs) {}

    /// Seed the clock. The first sync is due immediately, so the first STA window catches the account up.
    void begin(uint32_t nowMs) { dueAtMs_ = nowMs; }

    /// True once the interval has elapsed since the last successful sync (or since begin(), for the
    /// first). Wrap-safe across a millis() rollover.
    bool isDue(uint32_t nowMs) const { return static_cast<int32_t>(nowMs - dueAtMs_) >= 0; }

    /// Record a successful sync at @p nowMs, pushing the next due time out by one interval.
    void noteSynced(uint32_t nowMs) { dueAtMs_ = nowMs + intervalMs_; }

private:
    uint32_t intervalMs_;
    uint32_t dueAtMs_ = 0;
};

}  // namespace sapper
