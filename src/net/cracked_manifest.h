/**
 * @file cracked_manifest.h
 * @brief The bounded, per-BSSID account mirror over a CrackedStore (ADR-0019 decisions #4, #5).
 *
 * Pure policy: it owns no filesystem. It holds the account's cracked set keyed by AP BSSID, decides
 * what applying one downloaded result means — a new AP, an AP whose password changed (a rotated PSK),
 * or one already held unchanged — and, when full, which entry to evict. The device backs the store
 * with LittleFS and a host test backs it with an in-RAM fake, so the whole policy is unit-tested on
 * the native lane (ADR-0004 lane 1; slice-0020 Scenarios C, D, E, G).
 *
 * The apply outcome IS the "delta" ADR-0019 decision #5 describes: the sync raises a new-password
 * event exactly when apply() reports New or Changed, and stays silent on Unchanged. Keeping that
 * classification in the one place that also mutates the map means the alert decision and the stored
 * state can never disagree — a separate detector re-deriving the same lookup could drift from it.
 *
 * Entries live in a fixed array (no heap, like the rest of the capture/upload path); the manifest is
 * loaded whole on begin() and rewritten whole on flush() after a successful sync. The freshManifest
 * flag distinguishes a never-synced account from one synced-and-found-empty (decision #5). When the
 * account exceeds kMaxCrackedEntries the oldest crack is evicted; wpa-sec still holds it and a later
 * sync re-seeds it (decision #4) — see the LEDGER item on churn above that bound.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/cracked_lookup.h"
#include "net/cracked_store.h"
#include "net/reverification.h"  // reverifyDue(): the rotated-PSK staleness test (ADR-0072).

namespace sapper {

/// The most cracked APs the manifest mirrors in RAM. A hard bound (ADR-0019 decision #4): a no-PSRAM
/// target cannot hold an unbounded account in RAM. At sizeof(CrackedEntry) each this is tens of KB,
/// comparable to the upload path's pcap buffer. An account larger than this churns its oldest entries
/// (LEDGER); measure real account sizes before raising it.
constexpr size_t kMaxCrackedEntries = 256;

/// What applying one downloaded result did — the edge the sync alerts on (ADR-0019 decision #5).
/// New: a BSSID not held. Changed: a held BSSID whose password differs (rotated PSK). Unchanged: a
/// held BSSID with the same password (a re-download of a known crack — silent).
enum class ApplyOutcome { New, Changed, Unchanged };

/**
 * @brief Applies the per-BSSID mirror + bound + oldest-eviction policy over an injected CrackedStore.
 *
 * Construct with the store; call begin() once at boot to load, apply() per downloaded result during a
 * sync, clearFresh() after the first sync, and flush() once to persist a completed sync. apply() and
 * clearFresh() mutate only RAM so a whole sync is one store write, and a failed sync that never
 * flushes leaves the persisted manifest exactly as it was (ADR-0019 decision #7; Scenario I).
 */
class CrackedManifest : public CrackedLookup {
public:
    explicit CrackedManifest(CrackedStore& store) : store_(store) {}

    /// Load the persisted manifest and fresh flag from the store. Returns false on a store error
    /// (fail loud — the sync must not run against an unknown prior state).
    bool begin();

    /// Apply one downloaded result into the RAM map, returning what it meant for alerting. On New at
    /// the bound, evicts the entry with the oldest firstSeenCrackedMs. On Changed, updates the
    /// password/essid in place and keeps the original firstSeenCrackedMs. On Unchanged, does nothing.
    ApplyOutcome apply(const CrackedResult& result, uint32_t nowMs);

    /// True until the first sync completes — a never-yet-synced account, so a first sync seeds silently
    /// and summarises rather than alerting per entry (ADR-0019 decision #5). Persisted via the store.
    bool isFresh() const { return fresh_; }

    /// Mark the manifest no longer fresh (RAM only; persisted by the next flush()). Called once, after
    /// the first successful sync has seeded and summarised.
    void clearFresh() { fresh_ = false; }

    /// Persist the current entries and fresh flag to the store. Returns false on a store error.
    bool flush();

    size_t size() const { return count_; }

    /// The entry for @p bssid, or nullptr if not held. Read-only view for the sync/verify; the manifest
    /// stays the store's single writer (§4 invariant #12).
    const CrackedEntry* find(const uint8_t bssid[6]) const;

    /// CrackedLookup (ADR-0059): "have we already recovered this BSSID's PSK?" — manifest membership
    /// over the same find() read path, so the upload/targeting gates never grow a parallel parse. Now
    /// re-verification-aware (ADR-0072 decision 2): a held entry gone stale against the cached verification
    /// clock answers *false*, so gate A (drain drop) and gate B (target skip) both re-enable it for one
    /// re-capture/re-upload pass. Inert until setVerificationClock is fed a real epoch (kMinRealEpoch
    /// guard), so the pre-cadence behaviour holds verbatim before the first NTP sync.
    bool isCracked(const uint8_t bssid[6]) const override {
        const CrackedEntry* e = find(bssid);
        return e != nullptr && !reverifyDue(e->lastVerifiedEpoch, nowEpoch_);
    }

    /// Feed the current wall-clock epoch for the re-verification cadence (ADR-0072 decision 3). The device
    /// loop sets it from std::time(nullptr); the pure manifest never reads a clock itself (host-testable).
    /// App-task only, like every other manifest access (§4 #8/#10 untouched — never on the onFrame path).
    void setVerificationClock(uint32_t nowEpoch) { nowEpoch_ = nowEpoch; }

    /// The @p i-th mirrored entry (0..size()-1), or nullptr if out of range. Lets a surface enumerate
    /// the whole account — the Maintenance dashboard (ADR-0039) — through this seam rather than parsing
    /// the store itself, so the manifest stays the store's single reader/writer (§4 invariant #12).
    const CrackedEntry* entryAt(size_t i) const;

private:
    /// Index of the entry for @p bssid, or count_ if absent.
    size_t indexOf(const uint8_t bssid[6]) const;
    /// Index of the entry with the oldest firstSeenCrackedMs — the eviction victim at the bound.
    size_t oldestIndex() const;

    CrackedStore& store_;
    CrackedEntry entries_[kMaxCrackedEntries];
    size_t count_ = 0;
    bool fresh_ = true;
    uint32_t nowEpoch_ = 0;  ///< cached wall-clock for the re-verification cadence (ADR-0072); 0 = no real clock yet.
};

}  // namespace sapper
