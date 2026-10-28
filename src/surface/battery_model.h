/**
 * @file battery_model.h
 * @brief The pure battery interpretation: raw pin millivolts -> a coarse charge bucket (ADR-0091).
 *
 * This is the "pure core" half of §4 #40: the BatteryGauge seam (battery_gauge.h) forwards raw calibrated
 * millivolts, and everything that turns that into a charge level lives here, host-tested on the native
 * lane. Two nonlinearities are handled: the board's resistor divider (a per-board ratio, applied to reach
 * cell millivolts) and the coarse, honest bucketing a single voltage reading can support (the Li-ion
 * discharge curve is nearly flat mid-range, so a percentage would be false precision; a few levels is
 * what the reading actually carries). A small hysteresis band keeps the bucket from flickering when the
 * voltage dithers at a boundary. All integer math: no float, no heap, fits the no-PSRAM budget (ADR-0001).
 */
#pragma once

#include <cstdint>

#include "surface/battery_gauge.h"  // BatteryReading — the raw input this interprets.

namespace sapper {

/// The coarse charge level shown on the HUD. Ordered from empty to full; Unknown is the fail-safe for an
/// uncalibrated board (divider 0) or an absent gauge, never a guessed level (measure-don't-estimate).
enum class BatteryLevel : uint8_t { Unknown, Critical, Low, Mid, High, Full };

/// Architecture-derived Li-ion bucket boundaries, in cell millivolts (tuning from a real discharge curve
/// deferred, LEDGER, like the dwell/backoff constants). Each is the voltage at or above which that level
/// begins; below kLowFloorMv is Critical.
constexpr uint16_t kLowFloorMv = 3400;   ///< >= this is Low (below is Critical).
constexpr uint16_t kMidFloorMv = 3600;   ///< >= this is Mid.
constexpr uint16_t kHighFloorMv = 3800;  ///< >= this is High.
constexpr uint16_t kFullFloorMv = 4000;  ///< >= this is Full.

/// The hysteresis half-band: the bucket moves up only once the reading clears a boundary by this margin,
/// and down only once it falls below by this margin, so a voltage sitting on a threshold does not flicker.
constexpr uint16_t kBatteryHysteresisMv = 40;

/// Scale a raw pin reading to cell millivolts through the board's divider (permille: 2000 = x2.000). A
/// divider of 0 means "uncalibrated" and has no meaningful cell voltage; callers treat it as Unknown.
/// Returns a uint32 deliberately: a mis-authored divider (e.g. 20000 for 2000) then yields an absurd but
/// faithful value (66000 mV) that fails loud downstream (the [BATT] line + verify:battery range check),
/// instead of a uint16 that would wrap a 66000 to a plausible-looking 464 mV and silently misreport charge.
uint32_t batteryCellMilliVolts(uint16_t pinMilliVolts, uint16_t dividerPermille);

/// Bucket a cell voltage into a BatteryLevel, applying hysteresis relative to @p previous. On the first
/// reading (previous == Unknown) the plain bucket is returned; thereafter a one-step change is accepted
/// only once the reading has cleared the crossed boundary by kBatteryHysteresisMv (a large jump, e.g. a
/// cell swap, snaps straight to the plain bucket).
BatteryLevel batteryLevelFromCell(uint32_t cellMilliVolts, BatteryLevel previous);

/// The full interpretation: an absent gauge or an uncalibrated divider (0) is Unknown; otherwise scale to
/// cell millivolts and bucket with hysteresis. This is the one entry point the surface calls.
BatteryLevel batteryLevel(const BatteryReading& reading, uint16_t dividerPermille, BatteryLevel previous);

/// A short upper-case name for the serial `[BATT]` diagnostic line (the machine-checkable verify half).
const char* batteryLevelName(BatteryLevel level);

}  // namespace sapper
