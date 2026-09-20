/**
 * @file panel_probe.h
 * @brief Bench probe for the panel-parameter proof (ADR-0041, formerly the shipped boot face). Device-only.
 *
 * Renders the RGB-blocks + white border + corner-to-corner diagonal diagnostic — the frame that
 * confirms the ADR-0002 §5 panel parameters (colour/byte order, offset/clipping, mirror/rotation) —
 * so the serial `dump` captures it for `verify:device` to reconstruct into a PNG. It was the shipped
 * boot face through slice-0005; slice-0042 replaced that face with a product splash and relocated the
 * diagnostic here, behind SAPPER_TEST_PANEL, so no shipped build renders the test pattern (§4 #22).
 * Like every probe it is compiled out of every build without SAPPER_TEST_HOOKS (§4 #4). A screenless
 * board has nothing to prove, so the probe declines there.
 */
#pragma once

namespace sapper {

class IDisplay;

/// Start the panel proof if SAPPER_TEST_PANEL is set; returns false (normal boot) otherwise. Takes the
/// caller's panel (the one main.cpp created and the serial `dump` reads) so there is one canvas — the
/// diagnostic must render to the same sprite the dump streams back, exactly as the other panel probes do.
bool panelProbeBegin(IDisplay& display);

/// Whether the panel probe owns the device this run.
bool panelProbeActive();

/// Keep the device parked on the diagnostic frame, emitting a heartbeat while it waits for `dump`.
void panelProbePump();

}  // namespace sapper
