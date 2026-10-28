/**
 * @file battery_gauge_esp32.h
 * @brief The on-device battery gauge behind the BatteryGauge seam (ADR-0091, ADR-0002). Device-only.
 *
 * Reads the active board's battery ADC pin via the Arduino core's calibrated analogReadMilliVolts(), and
 * forwards the raw pin voltage only (§4 #40): no divider scaling, no bucketing. All board facts (the ADC
 * pin, and whether the board has a gauge at all) come from the Board Profile (ADR-0002), so this driver
 * hardcodes no pin. A board whose batteryAdcPin is -1 has no gauge and reports present = false, the
 * battery analogue of a screenless board resolving to a NullDisplay.
 */
#pragma once

#ifndef UNIT_TEST

#include "config/board_profile.h"
#include "surface/battery_gauge.h"

namespace sapper {

class Esp32BatteryGauge : public BatteryGauge {
public:
    explicit Esp32BatteryGauge(const BoardProfile& profile) : profile_(profile) {}

    BatteryReading read() override;

private:
    /// A small odd sample count whose median rejects the occasional ADC outlier without stalling the loop.
    static constexpr int kSamples = 5;

    const BoardProfile& profile_;
};

}  // namespace sapper

#endif  // UNIT_TEST
