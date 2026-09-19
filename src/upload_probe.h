/**
 * @file upload_probe.h
 * @brief Bench scaffolding for the slice-0018 on-air verify: run the shipped hunt+upload loop with a
 *        stimulus-injected handshake and report the drain over serial (ADR-0017). Device-only.
 *
 * The shipped loop (hunt_loop.h) hunts and uploads on its own, but a natural upload needs a real
 * (deauth-forced) handshake, which is deferred. This probe lets `verify:upload` prove the
 * capture→store→TLS-upload→resume path now: gated behind SAPPER_TEST_HOOKS, active only when
 * SAPPER_TEST_UPLOAD is set, it starts the real spine, injects one synthetic wpa-sec-valid handshake
 * through the capture-ready seam, and drives the drain so the TLS handshake against the pinned root
 * and the wpa-sec round-trip happen against the live service. Compiled to no-op stubs in every shipped
 * build (§4 invariant #4).
 */
#pragma once

namespace sapper {

/// Start the verify loop if SAPPER_TEST_UPLOAD is set. Returns true when the probe took over the
/// device (main.cpp then skips the normal boot); false when inactive or in a build without hooks.
bool uploadProbeBegin();

/// Whether the probe is running and owns loop().
bool uploadProbeActive();

/// Pump once per loop(): advance the hunt+drain loop and report the drain outcome. No-op unless active.
void uploadProbePump();

}  // namespace sapper
