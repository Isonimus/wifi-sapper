/**
 * @file capture_probe.h
 * @brief Bench probe for the slice-0032 capture-notification verify (ADR-0031). Device-only.
 *
 * Boots the device straight into the full hunt-loop surface stack and injects a capture stimulus on a
 * cadence, so the SerialEventLogger emits a repeating `[CAPTURE]` line and the panel shows the CAPTURED
 * banner for a serial `dump`. It proves the two device-only halves a host test cannot: the
 * SerialEventLogger's HandshakeCaptured case and the Esp32ScreenRenderer's CAPTURED label. Like the
 * screen probe it needs no network — a capture is announced at enqueue, before any drain — and like all
 * probes it is inactive unless SAPPER_TEST_CAPTURE is set and is compiled out of every shipped build
 * (§4 invariant #4).
 */
#pragma once

namespace sapper {

class IDisplay;

/// Start the capture-notify verify if SAPPER_TEST_CAPTURE is set; returns false (normal boot) otherwise.
/// Takes the caller's panel (the one main.cpp created and the serial `dump` reads) so the surface renders
/// to the same canvas the dump streams back, exactly like the screen probe.
bool captureProbeBegin(IDisplay& display);

/// Whether the capture probe owns the device this run.
bool captureProbeActive();

/// Pump the hunt loop (which renders the HUD + banner) and keep re-injecting a capture so the CAPTURED
/// banner is lit whenever `dump` arrives.
void captureProbePump();

}  // namespace sapper
