/**
 * @file cracked_store.h
 * @brief The persistence seam under the cracked-results manifest (ADR-0019 decision #4).
 *
 * The pure CrackedManifest policy (cracked_manifest.h) talks only to this abstract store; it never
 * opens a file. The device backs it with LittleFS on internal flash (cracked_store_littlefs.h) so the
 * account mirror survives the routine power loss of a portable appliance, and a host test backs it
 * with an in-RAM fake (test/support/fake_cracked_store.h) — the same seam split the CaptureStore uses
 * (ADR-0017 decision #3). This is the seam §4 invariant #12 names: cracked passwords are read and
 * written only through it, so no surface grows its own parse of the store.
 *
 * Unlike the CaptureStore (a set of independently-added/removed pcap blobs), the manifest is one small
 * map read and rewritten whole: load() on boot, save() once per successful sync. Two things persist —
 * the entries and the freshManifest flag — because the flag is what tells a first sync of a never-yet-
 * synced account (seed silently + summarise) apart from an ordinary sync that happens to find the
 * manifest empty (ADR-0019 decision #5). It is deliberately NOT where lastSuccessfulSyncMs lives: that
 * is a millis() value the SyncScheduler holds in RAM, meaningless across the reboot that resets millis.
 *
 * Every operation fails loud (quality bar §3): a corrupt or unwritable flash surfaces as an error the
 * sync counts and reports, never a manifest silently reported persisted that was not. A *missing* file
 * is not an error — it is a fresh, empty manifest (count 0, freshManifest true), the first-boot state.
 */
#pragma once

#include <cstddef>

#include "net/cracked_result.h"

namespace sapper {

/// One AP's mirrored crack: the parsed result plus when this appliance first recorded it cracked.
/// firstSeenCrackedMs is the eviction key when the manifest is at its bound (ADR-0019 decision #4).
struct CrackedEntry {
    CrackedResult result;              ///< BSSID (the map key), ESSID, and the recovered password.
    uint32_t firstSeenCrackedMs = 0;   ///< millis() when first stored; lower == older for eviction.
};

/// Where the account's cracked mirror persists between syncs. Backed by LittleFS on-device and an
/// in-RAM fake on the host lane. All methods fail loud (quality bar §3).
class CrackedStore {
public:
    virtual ~CrackedStore() = default;

    /// Load the persisted manifest into @p out (capacity @p capacity), setting @p count and
    /// @p freshManifest from the stored flag. A missing manifest is success with count 0 and
    /// freshManifest true (first-boot). Returns false only on a real store error, or if more entries
    /// are persisted than @p capacity holds — an overrun is a fault to surface, not to truncate.
    virtual bool load(CrackedEntry* out, size_t capacity, size_t& count, bool& freshManifest) = 0;

    /// Persist @p count entries and the @p freshManifest flag as one whole manifest, replacing any
    /// prior contents. Returns false on any write failure, having left the prior manifest intact where
    /// the backing can guarantee it (never a half-written manifest reported as saved).
    virtual bool save(const CrackedEntry* entries, size_t count, bool freshManifest) = 0;
};

}  // namespace sapper
