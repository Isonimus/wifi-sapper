/**
 * @file deauth_probe.h
 * @brief Bench probe for the slice-0028 deauth verify (ADR-0027). Device-only, SAPPER_TEST_HOOKS.
 *
 * Transmits deauth/disassoc frames at a target AP and captures the handshake the deauth forces —
 * the on-air proof ADR-0009 #4 deferred. Inactive unless SAPPER_TEST_DEAUTH_BSSID is set, and
 * compiled to no-ops without SAPPER_TEST_HOOKS (the raw-TX capability is absent from shipped
 * binaries, ADR-0027 #3). Dispatched from main.cpp's setup()/loop() like the other bench probes.
 */
#pragma once

namespace sapper {

/// Start the deauth probe if SAPPER_TEST_DEAUTH_BSSID is set. Returns true when it took over the
/// device (the caller then skips normal boot), false when inactive or on a fatal setup error.
bool deauthProbeBegin();

/// Whether the probe owns the device this run.
bool deauthProbeActive();

/// Transmit the next deauth burst and report TX / self-heard / capture status. Call from loop().
void deauthProbePump();

}  // namespace sapper
