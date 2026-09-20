/**
 * @file webhook_capture_probe.h
 * @brief Bench scaffolding for the slice-0036 on-air capture-push verify: run the shipped hunt loop with
 *        capture-push forced on, inject a synthetic capture fact, force an STA window, and let the
 *        transport POST it to the live push service (ADR-0035). Device-only.
 *
 * The webhook now fires on a fresh capture too (ADR-0035), gated by the operator's per-type selector.
 * The live HTTPS POST path is unchanged from slice-0024, so this probe — a sibling of the slice-0024
 * webhook probe, gated behind SAPPER_TEST_HOOKS and active only when SAPPER_TEST_WEBHOOK_CAPTURE is set —
 * forces `notifyCaptured` on in RAM (so the proof does not depend on the provisioned box), starts the
 * real spine, injects a synthetic HandshakeCaptured through the bus, and forces a due sync so the next
 * STA window flushes it. The transport logs `[WEBHOOK] sent … code=2xx`, the machine-checkable proof.
 * Compiled to no-op stubs in every shipped build (§4 invariant #4).
 */
#pragma once

namespace sapper {

/// Start the capture-push verify loop if SAPPER_TEST_WEBHOOK_CAPTURE is set. Returns true when the probe
/// took over the device (main.cpp then skips the normal boot); false when inactive or in a no-hooks build.
bool webhookCaptureProbeBegin();

/// Whether the probe is running and owns loop().
bool webhookCaptureProbeActive();

/// Pump once per loop(): advance the hunt loop and re-drive the STA window until a capture has POSTed.
void webhookCaptureProbePump();

}  // namespace sapper
