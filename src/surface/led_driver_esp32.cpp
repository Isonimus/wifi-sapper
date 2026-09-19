/**
 * @file led_driver_esp32.cpp
 * @brief On-device status-LED rendering (ADR-0021). Device-only.
 */
#include "surface/led_driver_esp32.h"

#ifndef UNIT_TEST

#include <Arduino.h>

namespace sapper {
namespace {

/// One RGB colour for the driver to write. A single-LED board reads only whether it is lit (any
/// non-zero channel), so the colour table doubles as the on/off table.
struct Rgb {
    uint8_t r, g, b;
};

/// WS2812s are eye-searingly bright at full scale and this is an unattended appliance, so every colour
/// is scaled to a modest level — bright enough to read across a room, dim enough to run on battery.
constexpr uint8_t kLevel = 24;

/// The status → colour map (ADR-0021 vocabulary). Amber is full red + a third green.
Rgb colorOf(LedStatus status) {
    switch (status) {
        case LedStatus::Off:       return {0, 0, 0};
        case LedStatus::Hunting:   return {0, kLevel, 0};             // calm green: alive and hunting.
        case LedStatus::Working:   return {0, 0, kLevel};             // blue: off-air, uploading/syncing.
        case LedStatus::Degraded:  return {kLevel, kLevel / 3, 0};    // amber: last drain failed.
        case LedStatus::Fault:     return {kLevel, 0, 0};             // red: hard fault.
        case LedStatus::Recovered: return {kLevel, kLevel, kLevel};   // white flash: new password.
    }
    return {0, 0, 0};  // unreachable; keeps the compiler from warning on a non-void path.
}

const char* nameOf(LedStatus status) {
    switch (status) {
        case LedStatus::Off:       return "off";
        case LedStatus::Hunting:   return "hunting";
        case LedStatus::Working:   return "working";
        case LedStatus::Degraded:  return "degraded";
        case LedStatus::Fault:     return "fault";
        case LedStatus::Recovered: return "recovered";
    }
    return "?";
}

}  // namespace

void Esp32LedDriver::begin() {
    // Only a plain single LED needs its GPIO put in output mode; the RGB path uses rgbLedWrite (RMT),
    // and a headless board has no pin to configure.
    if (profile_.led == LedKind::Single && profile_.ledPin >= 0) {
        pinMode(static_cast<uint8_t>(profile_.ledPin), OUTPUT);
    }
}

void Esp32LedDriver::show(LedStatus status) {
    // The serial line is the verify's machine-checkable evidence of a status change, emitted whatever
    // the board's LED hardware is (ADR-0021 / ADR-0004 lane 3). Log only a genuine semantic change:
    // the heartbeat toggles the display Off/on every second, and logging each toggle would bury the
    // real transitions (and spam the shipped build's serial). The physical LED below still blinks.
    if (status != LedStatus::Off && status != lastLogged_) {
        Serial.printf("[LED] status=%s\n", nameOf(status));
        lastLogged_ = status;
    }

    const Rgb color = colorOf(status);
    switch (profile_.led) {
        case LedKind::Rgb:
            if (profile_.ledPin >= 0) {
                // Arduino-ESP32's built-in single-WS2812 writer (RMT-backed) — no extra library.
                rgbLedWrite(static_cast<uint8_t>(profile_.ledPin), color.r, color.g, color.b);
            }
            break;
        case LedKind::Single:
            if (profile_.ledPin >= 0) {
                const bool lit = color.r != 0 || color.g != 0 || color.b != 0;
                digitalWrite(static_cast<uint8_t>(profile_.ledPin), lit ? HIGH : LOW);
            }
            break;
        case LedKind::None:
            break;  // headless board: the serial line above is the only surface.
    }
}

}  // namespace sapper

#endif  // UNIT_TEST
