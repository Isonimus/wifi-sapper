/**
 * @file sync_probe.h
 * @brief Bench scaffolding for the slice-0020 on-air sync verify: run the shipped hunt loop, force a
 *        cracked-results sync due without waiting a real hour, and report it over serial (ADR-0019).
 *        Device-only.
 *
 * The shipped loop (hunt_loop.h) syncs the account's cracked set once an hour, piggybacking the drain's
 * STA window. A bench verify cannot wait an hour, so this probe — gated behind SAPPER_TEST_HOOKS, active
 * only when SAPPER_TEST_SYNC is set — starts the real spine, arms a due sync via the clock-advance
 * stimulus (huntLoopForceSyncDue), and drives the loop so the pinned-TLS `GET /?api&dl=1`, the HTTPClient
 * de-chunk, the parse, and the LittleFS manifest seed all happen against the live service (Scenario J).
 * Compiled to no-op stubs in every shipped build (§4 invariant #4).
 */
#pragma once

namespace sapper {

/// Start the sync-verify loop if SAPPER_TEST_SYNC is set. Returns true when the probe took over the
/// device (main.cpp then skips the normal boot); false when inactive or in a build without hooks.
bool syncProbeBegin();

/// Whether the probe is running and owns loop().
bool syncProbeActive();

/// Pump once per loop(): advance the hunt loop and report each completed sync. No-op unless active.
void syncProbePump();

}  // namespace sapper
