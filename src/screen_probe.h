/**
 * @file screen_probe.h
 * @brief Bench probe for the slice-0026 screen-toast verify (ADR-0025). Device-only.
 *
 * Boots the device straight into the status-HUD/toast surface with a synthetic crack armed, so the
 * serial `dump` captures a frame showing the HUD and the CRACKED banner for human review. Like the
 * other probes it is inactive unless SAPPER_TEST_SCREEN is set and is compiled out of every shipped
 * build (§4 invariant #4). Unlike the LED probe it needs no network: rendering a local panel is
 * independent of connectivity, so the verify runs without WiFi/wpa-sec credentials.
 */
#pragma once

namespace sapper {

class IDisplay;

/// Start the screen verify if SAPPER_TEST_SCREEN is set; returns false (normal boot) otherwise. Takes
/// the caller's panel (the one main.cpp created and the serial `dump` reads) so there is one canvas —
/// the surface must render to the same sprite the dump streams back, or the shot would miss the toast.
bool screenProbeBegin(IDisplay& display);

/// Whether the screen probe owns the device this run.
bool screenProbeActive();

/// Pump the hunt loop (which renders the HUD) and keep the CRACKED banner armed for the dump.
void screenProbePump();

}  // namespace sapper
