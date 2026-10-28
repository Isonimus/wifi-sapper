/**
 * @file fake_battery_gauge.h
 * @brief A BatteryGauge double for the host lane (ADR-0091; slice-0092).
 *
 * Lets a test drive the screen surface's battery sampling without a real ADC: set `reading` to whatever
 * presence/pin-voltage the scenario needs, and the surface pulls it each sample exactly as it pulls the
 * real Esp32BatteryGauge. `reads` counts the pulls so a test can assert the sample throttle.
 */
#pragma once

#include "surface/battery_gauge.h"

namespace sapper_test {

class FakeBatteryGauge : public sapper::BatteryGauge {
public:
    sapper::BatteryReading reading;
    int reads = 0;

    sapper::BatteryReading read() override {
        ++reads;
        return reading;
    }
};

}  // namespace sapper_test
