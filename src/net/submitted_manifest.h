/**
 * @file submitted_manifest.h
 * @brief The bounded, per-BSSID "submitted to wpa-sec" set over a SubmittedStore (ADR-0065 gate C).
 *
 * The local twin of CrackedManifest (cracked_manifest.h): pure policy, owns no filesystem, holds the
 * set of BSSIDs this appliance has landed a valid handshake for at wpa-sec, keyed by AP BSSID. Leaner
 * than the cracked manifest — a SubmittedEntry carries no ESSID or PSK (a submitted-but-not-cracked
 * network has none), and there is no fresh flag (submitted has no first-sync semantics; see
 * submitted_store.h). The device backs the store with LittleFS and a host test backs it with an in-RAM
 * fake, so the whole policy is unit-tested on the native lane.
 *
 * It is loaded whole on begin() and rewritten whole on flushSubmitted() at the end of a drain cycle that
 * recorded a submission. When the set exceeds kMaxSubmittedEntries the oldest submission (lowest
 * uploadedAt) is evicted; the network then becomes targetable again and is re-captured (a bounded churn
 * at the bound, like the cracked manifest's oldest-crack eviction, ADR-0019 decision #4).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/reverification.h"  // reverifyDue(): the rotated-PSK staleness test (ADR-0072).
#include "net/submitted_lookup.h"
#include "net/submitted_store.h"

namespace sapper {

/// The most submitted APs the set mirrors in RAM. A hard bound (ADR-0065 #1): a no-PSRAM target cannot
/// hold an unbounded set. A SubmittedEntry is a 10-byte record on disk (bssid[6] + u32) — ~12 bytes in
/// RAM once padded — so the whole set is a few KB, far smaller than the cracked manifest (which carries
/// the ESSID + PSK per entry). Sized to match kMaxCrackedEntries so the submitted floor is never smaller
/// than the cracked one it generalises.
constexpr size_t kMaxSubmittedEntries = 256;

/**
 * @brief Applies the per-BSSID set + bound + oldest-eviction policy over an injected SubmittedStore.
 *
 * Construct with the store; call begin() once at boot to load, markSubmitted() per terminal upload
 * success during a drain, and flushSubmitted() once at the end of a drain that marked anything.
 * markSubmitted() mutates only RAM so a whole drain is one store write, and a drain that never flushes
 * leaves the persisted set exactly as it was.
 */
class SubmittedManifest : public SubmittedRecorder {
public:
    explicit SubmittedManifest(SubmittedStore& store) : store_(store) {}

    /// Load the persisted set from the store. Returns false on a store error (fail loud — the caller
    /// must treat gate C as unavailable this boot rather than run against an unknown prior state).
    bool begin();

    /// SubmittedRecorder (ADR-0065 #3): record @p bssid submitted. If already held, keeps the original
    /// uploadedAt (idempotent membership). If new at the bound, evicts the entry with the oldest
    /// uploadedAt. RAM only; persisted by the next flushSubmitted().
    void markSubmitted(const uint8_t bssid[6], uint32_t nowMs) override;

    /// SubmittedLookup (ADR-0065 #2): "have we already landed a valid handshake for this BSSID?" —
    /// set membership. The engine's gate-C target filter and the drain's gate-C drop read this. Now
    /// re-verification-aware (ADR-0072 decision 2): a held entry gone stale against the cached verification
    /// clock answers *false*, re-enabling gate C at both sites for one re-capture/re-upload. Inert until
    /// setVerificationClock is fed a real epoch (kMinRealEpoch guard) — pre-cadence behaviour before NTP.
    bool isSubmitted(const uint8_t bssid[6]) const override;

    /// Feed the current wall-clock epoch for the re-verification cadence (ADR-0072 decision 3). The device
    /// loop sets it from std::time(nullptr); the pure manifest never reads a clock itself (host-testable).
    /// App-task only, like every other manifest access (§4 #8/#10 untouched — never on the onFrame path).
    void setVerificationClock(uint32_t nowEpoch) { nowEpoch_ = nowEpoch; }

    /// Persist the current entries to the store. Returns false on a store error.
    bool flushSubmitted() override;

    size_t size() const { return count_; }

private:
    /// Index of the entry for @p bssid, or count_ if absent.
    size_t indexOf(const uint8_t bssid[6]) const;
    /// Index of the entry with the oldest uploadedAt — the eviction victim at the bound.
    size_t oldestIndex() const;

    SubmittedStore& store_;
    SubmittedEntry entries_[kMaxSubmittedEntries];
    size_t count_ = 0;
    uint32_t nowEpoch_ = 0;  ///< cached wall-clock for the re-verification cadence (ADR-0072); 0 = no real clock yet.
};

}  // namespace sapper
