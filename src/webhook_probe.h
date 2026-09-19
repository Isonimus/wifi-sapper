/**
 * @file webhook_probe.h
 * @brief Bench scaffolding for the slice-0024 on-air webhook verify: run the shipped hunt loop, inject a
 *        synthetic crack, force an STA window, and let the transport POST it to the live push service
 *        (ADR-0023). Device-only.
 *
 * The push-notification surface (webhook_notifier.h) observes a NewPassword on the bus and POSTs it
 * inside an STA window through a TLS transport verified against the system CA bundle — a path only
 * hardware and a real endpoint can prove. This probe — gated behind SAPPER_TEST_HOOKS, active only when
 * SAPPER_TEST_WEBHOOK is set — starts the real spine (which wires the webhook from the provisioned URL),
 * injects a synthetic new-password fact through the bus (so a notification is enqueued), and forces a
 * due sync so the next STA window flushes it. The transport logs `[WEBHOOK] sent … code=2xx`, the
 * machine-checkable proof. Compiled to no-op stubs in every shipped build (§4 invariant #4).
 */
#pragma once

namespace sapper {

/// Start the webhook-verify loop if SAPPER_TEST_WEBHOOK is set. Returns true when the probe took over
/// the device (main.cpp then skips the normal boot); false when inactive or in a build without hooks.
bool webhookProbeBegin();

/// Whether the probe is running and owns loop().
bool webhookProbeActive();

/// Pump once per loop(): advance the hunt loop and re-drive the webhook stimulus. No-op unless active.
void webhookProbePump();

}  // namespace sapper
