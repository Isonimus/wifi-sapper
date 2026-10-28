/**
 * @file battery_gauge.h
 * @brief The battery-read HAL seam: a raw calibrated millivolt reading, nothing interpreted (ADR-0091).
 *
 * The input-peripheral sibling of the status-LED's output seam (led_driver.h, ADR-0021): it keeps the
 * battery *hardware* (an ADC pin + the Arduino core's per-chip calibration) behind an abstract class, so
 * the battery *interpretation* (divider scaling, charge bucketing, hysteresis) stays in the pure,
 * host-tested BatteryModel. The seam forwards one raw receiver datum, the calibrated millivolts at the
 * pin, exactly as §4 #8's RX seam forwards raw bytes + rssi and adds no protocol logic: all policy lives
 * in the pure core, so porting to a new board is a new gauge plus board data, never new policy.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// One battery reading from the seam. `present` is false on a board with no gauge (batteryAdcPin < 0),
/// the analogue of a NullDisplay on a screenless board; `pinMilliVolts` is the Arduino core's calibrated
/// voltage AT THE PIN (post ADC-calibration, pre divider), meaningful only when present.
struct BatteryReading {
    bool present = false;
    uint16_t pinMilliVolts = 0;
};

/// Abstract battery gauge. read() is called from the app task on a throttle, never a callback context.
/// A fake implements it for the host tests, exactly as a fake HuntSnapshotSource drives the HUD.
class BatteryGauge {
public:
    virtual ~BatteryGauge() = default;

    /// Sample the gauge now and return the raw pin reading (or `present = false` if there is no gauge).
    virtual BatteryReading read() = 0;
};

}  // namespace sapper
