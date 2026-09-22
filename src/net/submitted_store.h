/**
 * @file submitted_store.h
 * @brief The persistence seam under the submitted-to-wpa-sec set (ADR-0065 gate C).
 *
 * The local twin of CrackedStore (net/cracked_store.h). The pure SubmittedManifest policy
 * (submitted_manifest.h) talks only to this abstract store; it never opens a file. The device backs it
 * with LittleFS on internal flash (submitted_store_littlefs.h) so the set survives the routine power
 * loss of a portable appliance, and a host test backs it with an in-RAM fake
 * (test/support/fake_submitted_store.h) — the same seam split CrackedStore/CaptureStore use.
 *
 * Like the cracked manifest, the submitted set is one small map read and rewritten whole: load() on
 * boot, save() once per drain cycle that recorded a submission. Unlike the cracked store there is NO
 * freshManifest flag — "submitted" has no first-sync/seed-silently semantics (that flag exists so a
 * first *download* sync summarises rather than alerting; a submission is a purely local event with no
 * such distinction). A missing file is not an error — it is an empty set (count 0), the first-boot state.
 *
 * Every operation fails loud (quality bar §3): a corrupt or unwritable flash surfaces as an error, never
 * a set silently reported persisted that was not. Losing this set is a degradation (gate C stops
 * filtering until re-populated), never a lost handshake — the cracked floor (§4 #27/#28) still holds.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// One submitted AP: the BSSID (the map key) and when this appliance first recorded it submitted.
/// uploadedAt is a millis() value — the eviction key when the set is at its bound (ADR-0065 #1), and
/// meaningful only for relative ordering within one boot (it resets across reboots, like the cracked
/// manifest's firstSeenCrackedMs; ADR-0065 consequence). It is deliberately NOT a re-verification timer.
struct SubmittedEntry {
    uint8_t bssid[6] = {0};
    uint32_t uploadedAt = 0;  ///< millis() when first recorded; lower == older for eviction.
};

/// Where the submitted set persists between drains. Backed by LittleFS on-device and an in-RAM fake on
/// the host lane. All methods fail loud (quality bar §3).
class SubmittedStore {
public:
    virtual ~SubmittedStore() = default;

    /// Load the persisted set into @p out (capacity @p capacity), setting @p count. A missing file is
    /// success with count 0 (first-boot). Returns false only on a real store error, or if more entries
    /// are persisted than @p capacity holds — an overrun is a fault to surface, not to truncate.
    virtual bool load(SubmittedEntry* out, size_t capacity, size_t& count) = 0;

    /// Persist @p count entries as one whole set, replacing any prior contents. Returns false on any
    /// write failure, having left the prior set intact where the backing can guarantee it (never a
    /// half-written set reported as saved).
    virtual bool save(const SubmittedEntry* entries, size_t count) = 0;
};

}  // namespace sapper
