/**
 * @file keyboard_probe.h
 * @brief Bench probe confirming the ADV Maintenance keycode on-device (ADR-0057). Device-only.
 *
 * Boots straight into a TCA8418 read loop that logs the decoded key number of every keypress, so the
 * operator presses `m` and reads back `code=48` — the measured confirmation ADR-0057 decision 5 needs
 * (a keycode is a measurable fact; the ADR records the measurement, not the guess). Like the other
 * probes it is inactive unless SAPPER_TEST_KB is set and is compiled out of every shipped build
 * (§4 invariant #4). Needs no network — reading the keypad is local.
 */
#pragma once

namespace sapper {

/// Start the keyboard probe if SAPPER_TEST_KB is set; returns false (normal boot) otherwise.
bool keyboardProbeBegin();

/// Whether the keyboard probe owns the device this run.
bool keyboardProbeActive();

/// Drain and log any key events, so a press of `m` prints its decoded key number for the operator.
void keyboardProbePump();

}  // namespace sapper
