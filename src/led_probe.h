/**
 * @file led_probe.h
 * @brief Bench scaffolding for the slice-0022 on-air LED verify: run the shipped hunt loop, drive the
 *        status LED through its states, and let the Esp32 driver report each over serial (ADR-0021).
 *        Device-only.
 *
 * The status LED (led_status_surface.h) is a bus subscriber wired into the shipped loop; its physical
 * colour can only be confirmed on hardware. This probe — gated behind SAPPER_TEST_HOOKS, active only
 * when SAPPER_TEST_LED is set — starts the real spine, forces an STA window (so the LED shows
 * working then returns to the hunting heartbeat) and injects a synthetic new-password fact through the
 * bus (so the LED shows the recovered flash). The Esp32 driver logs each status as `[LED] status=...`,
 * the machine-checkable half of the verify; the physical colours are the human half. Compiled to no-op
 * stubs in every shipped build (§4 invariant #4).
 */
#pragma once

namespace sapper {

/// Start the LED-verify loop if SAPPER_TEST_LED is set. Returns true when the probe took over the
/// device (main.cpp then skips the normal boot); false when inactive or in a build without hooks.
bool ledProbeBegin();

/// Whether the probe is running and owns loop().
bool ledProbeActive();

/// Pump once per loop(): advance the hunt loop and re-drive the LED stimulus. No-op unless active.
void ledProbePump();

}  // namespace sapper
