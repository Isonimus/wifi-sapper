/**
 * @file battery_gauge_esp32.cpp
 * @brief Implementation of the on-device battery gauge (ADR-0091). Device-only.
 */
#include "surface/battery_gauge_esp32.h"

#ifndef UNIT_TEST

#include <Arduino.h>

#include <algorithm>

namespace sapper {

BatteryReading Esp32BatteryGauge::read() {
    if (profile_.batteryAdcPin < 0) return {};  // no gauge on this board (present = false).

    // Median of a few calibrated reads: analogReadMilliVolts applies the chip's factory eFuse ADC
    // calibration and returns millivolts at the pin (the raw datum, §4 #40), and the median rejects the
    // occasional SAR outlier. The divider scaling and bucketing are the pure core's job, never here.
    uint16_t samples[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        samples[i] = static_cast<uint16_t>(analogReadMilliVolts(profile_.batteryAdcPin));
    }
    std::sort(samples, samples + kSamples);

    BatteryReading reading;
    reading.present = true;
    reading.pinMilliVolts = samples[kSamples / 2];
    return reading;
}

}  // namespace sapper

#endif  // UNIT_TEST
