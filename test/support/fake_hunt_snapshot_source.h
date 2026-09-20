/**
 * @file fake_hunt_snapshot_source.h
 * @brief A HuntSnapshotSource double for the host lane (ADR-0033; slice-0034).
 *
 * Lets a test drive the screen surface's live-hunt rendering without a real engine: set `snapshot` to
 * whatever phase/target/flags the scenario needs, and the surface pulls it each tick exactly as it pulls
 * the real HuntEngine.
 */
#pragma once

#include "net/hunt_snapshot.h"

namespace sapper_test {

class FakeHuntSnapshotSource : public sapper::HuntSnapshotSource {
public:
    sapper::HuntSnapshot snapshot;
    sapper::HuntSnapshot huntSnapshot() const override { return snapshot; }
};

}  // namespace sapper_test
