/**
 * @file hunt_hud_probe.h
 * @brief Bench probe for the slice-0034 live-hunt-HUD verify (ADR-0033). Device-only.
 *
 * Boots the device into the full hunt-loop surface stack and injects a synthetic beacon + M1/M2 through
 * the engine's real `onFrame` seam (§4 invariant #2), so the engine discovers, captures, and populates
 * its collector — lighting the panel's live-hunt line (target + Beacon/M1/M2 indicators + progress bar)
 * for a serial `dump`. It proves the device-only render path a host test cannot: the
 * `Esp32ScreenRenderer` drawing the live section from a real `HuntSnapshot`. Needs no network. Like all
 * probes it is inactive unless SAPPER_TEST_HUNT_HUD is set and is compiled out of every shipped build
 * (§4 invariant #4). Run it on a quiet channel so the synthetic AP is the one the engine captures.
 */
#pragma once

namespace sapper {

class IDisplay;

/// Start the hunt-HUD verify if SAPPER_TEST_HUNT_HUD is set; returns false (normal boot) otherwise.
/// Takes the caller's panel so the surface renders to the same canvas the serial `dump` reads.
bool huntHudProbeBegin(IDisplay& display);

/// Whether the hunt-HUD probe owns the device this run.
bool huntHudProbeActive();

/// Pump the hunt loop (which renders the live HUD) and keep injecting the synthetic AP + handshake so
/// the collector stays populated and the indicators stay lit whenever `dump` arrives.
void huntHudProbePump();

}  // namespace sapper
