/**
 * @file cracked_count_source.h
 * @brief The read-only pull seam the live HUD reads the lifetime cracked total through (ADR-0118).
 *
 * The hunt HUD's `cracked:` counter is the device's lifetime yield — how many networks it has ever
 * recovered — which is the persisted CrackedManifest count, not a run-scoped tally of NewPassword bus
 * facts (that resets on reboot and drifts on a rotated-PSK re-crack, ADR-0118). A surface reads it only
 * through this one-method seam, never by parsing the store (§4 invariant #12), exactly as it reads live
 * hunt state through HuntSnapshotSource (ADR-0033) and charge through BatteryGauge (ADR-0091).
 *
 * Pure (one abstract method), so the HUD policy is host-tested against a fake that returns a canned
 * count; CrackedManifest implements it on device over its existing size().
 */
#pragma once

#include <cstddef>

namespace sapper {

/// The seam a surface pulls the lifetime cracked total through. CrackedManifest implements it; a fake
/// implements it for the host tests. const: reading the count mutates no state.
class CrackedCountSource {
public:
    virtual ~CrackedCountSource() = default;

    /// How many networks the persisted manifest currently holds — the lifetime cracked total (ADR-0118).
    virtual size_t crackedTotal() const = 0;
};

}  // namespace sapper
