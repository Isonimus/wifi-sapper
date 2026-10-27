/**
 * @file button_hold.h
 * @brief Pure debounced button-hold decision for per-board Maintenance entry (ADR-0078).
 *
 * The button counterpart to `tca8418_event.h`'s keyboard decode: the pure half of a board's
 * Maintenance-entry gesture, host-tested on the native lane, with the `digitalRead`/`pinMode` glue
 * left to the device (`main.cpp`). A button is an electrical *level*, not the discrete FIFO event a
 * keypress is, so entry is a deliberate continuous *hold*, not a tap: a stray press during the endless
 * hunt must not reboot into Maintenance and expose recovered PSKs on the SoftAP (§4 #21, ADR-0078
 * decision 3).
 *
 * The decision is edge-firing and fail-safe: it returns true exactly once, on the tick the continuous
 * hold first reaches the threshold, never again while the button stays held, and resets on release.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// The hold duration that enters Maintenance. An architecture-derived default (hardware tuning deferred,
/// LEDGER), like dwell/settle/deauth-cadence/backoff. Long enough that a brush against the side button
/// during a hunt does not trip entry, short enough to be a comfortable deliberate press.
inline constexpr uint32_t kMaintenanceHoldMs = 2000;

/// Per-button hold state, owned by the caller (one instance per polled button). Default-constructs to
/// "not pressing, not yet fired", so a fresh state before the first poll never spuriously fires.
struct ButtonHoldState {
    bool pressing = false;        ///< Whether the previous poll saw the button down (for rising-edge detection).
    bool fired = false;           ///< Whether this press has already fired (so one hold is one entry, not a repeat).
    uint32_t pressStartMs = 0;    ///< `nowMs` at the rising edge; meaningful only while `pressing`.
};

/**
 * @brief Whether a continuous button hold has just reached the entry threshold.
 *
 * @param pressed  true when the button reads pressed this poll (active-low boards map LOW -> true).
 * @param nowMs    a monotonic millisecond clock (device: `millis()`).
 * @param holdMs   the continuous-hold duration required to enter (production: `kMaintenanceHoldMs`).
 * @param state    the caller-owned hold state, mutated across polls.
 * @return true on the single poll the hold first reaches `holdMs`; false otherwise.
 *
 * A rising edge records the start and does not fire; a release resets so a later hold fires again; while
 * the button stays held after firing it does not re-fire. The elapsed computation is unsigned
 * subtraction, so it stays correct across a `millis()` wrap for any hold shorter than the wrap period.
 */
bool decideButtonHold(bool pressed, uint32_t nowMs, uint32_t holdMs, ButtonHoldState& state);

}  // namespace sapper
