/**
 * @file battery_model.cpp
 * @brief Implementation of the pure battery interpretation (ADR-0091).
 */
#include "surface/battery_model.h"

namespace sapper {
namespace {

/// The plain bucket for a cell voltage, ignoring hysteresis: the thresholds applied top-down.
BatteryLevel plainLevel(uint32_t cellMilliVolts) {
    if (cellMilliVolts >= kFullFloorMv) return BatteryLevel::Full;
    if (cellMilliVolts >= kHighFloorMv) return BatteryLevel::High;
    if (cellMilliVolts >= kMidFloorMv) return BatteryLevel::Mid;
    if (cellMilliVolts >= kLowFloorMv) return BatteryLevel::Low;
    return BatteryLevel::Critical;
}

/// The lower edge (in cell millivolts) at or above which @p level begins. Critical starts at 0. Used to
/// place the hysteresis band on the boundary a one-step change is crossing.
uint16_t lowerEdge(BatteryLevel level) {
    switch (level) {
        case BatteryLevel::Full: return kFullFloorMv;
        case BatteryLevel::High: return kHighFloorMv;
        case BatteryLevel::Mid: return kMidFloorMv;
        case BatteryLevel::Low: return kLowFloorMv;
        case BatteryLevel::Critical: return 0;
        case BatteryLevel::Unknown: return 0;
    }
    return 0;
}

}  // namespace

uint32_t batteryCellMilliVolts(uint16_t pinMilliVolts, uint16_t dividerPermille) {
    // pin * permille / 1000, kept in uint32 end-to-end (no narrowing cast): a sane board yields a few
    // thousand mV, and a mis-authored divider yields an absurd value that fails loud rather than wrapping
    // to a plausible-looking one (see the header note). The product fits uint32 for any uint16 inputs.
    return static_cast<uint32_t>(pinMilliVolts) * dividerPermille / 1000;
}

BatteryLevel batteryLevelFromCell(uint32_t cellMilliVolts, BatteryLevel previous) {
    const BatteryLevel plain = plainLevel(cellMilliVolts);
    if (previous == BatteryLevel::Unknown || plain == previous) return plain;

    if (static_cast<uint8_t>(plain) > static_cast<uint8_t>(previous)) {
        // Moving up: the boundary being crossed is the lower edge of the next level up from previous.
        // Accept the change only once the reading has cleared that boundary by the hysteresis band. A
        // larger jump clears it comfortably and snaps to the plain bucket.
        const BatteryLevel oneUp = static_cast<BatteryLevel>(static_cast<uint8_t>(previous) + 1);
        const uint16_t boundary = lowerEdge(oneUp);
        return cellMilliVolts >= boundary + kBatteryHysteresisMv ? plain : previous;
    }
    // Moving down: the boundary is the lower edge of the previous level; accept only once the reading has
    // fallen below it by the band. boundary >= kLowFloorMv (3400) > the band, so no unsigned underflow.
    const uint16_t boundary = lowerEdge(previous);
    return cellMilliVolts < boundary - kBatteryHysteresisMv ? plain : previous;
}

BatteryLevel batteryLevel(const BatteryReading& reading, uint16_t dividerPermille, BatteryLevel previous) {
    if (!reading.present || dividerPermille == 0) return BatteryLevel::Unknown;
    return batteryLevelFromCell(batteryCellMilliVolts(reading.pinMilliVolts, dividerPermille), previous);
}

const char* batteryLevelName(BatteryLevel level) {
    switch (level) {
        case BatteryLevel::Full: return "FULL";
        case BatteryLevel::High: return "HIGH";
        case BatteryLevel::Mid: return "MID";
        case BatteryLevel::Low: return "LOW";
        case BatteryLevel::Critical: return "CRITICAL";
        case BatteryLevel::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

}  // namespace sapper
