/**
 * @file hunt_probe.h
 * @brief Bench scaffolding for the slice-0016 on-air verify: run the HuntEngine loop on real hardware
 *        and report its phases over serial (ADR-0015 decision #8). Device-only.
 *
 * There is no shipped hunt loop yet — with no uploader, an endless hunt would capture handshakes with
 * nowhere to send them (slice-5). This probe exists only so `verify:hunt` has real firmware to drive:
 * gated behind `SAPPER_TEST_HOOKS`, active only when `SAPPER_TEST_HUNT` (a hop channel list) is set,
 * and compiled to no-op stubs in every shipped build so no released binary hunts.
 */
#pragma once

namespace sapper {

/// Start the hunt loop if configured. Returns true when the probe took over the device (a hop list
/// was set and the engine began), meaning main.cpp should skip the normal boot; false when inactive
/// (no hop list configured) or in a build without test hooks.
bool huntProbeBegin();

/// Whether the probe is running and owns loop().
bool huntProbeActive();

/// Pump once per loop(): advance the engine's state machine and report each phase transition, each
/// discovered target, and any capture, plus a liveness heartbeat. No-op unless the probe is active.
void huntProbePump();

}  // namespace sapper
