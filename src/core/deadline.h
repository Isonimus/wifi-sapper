/**
 * @file deadline.h
 * @brief The wrap-safe "has this deadline arrived?" test, shared across the clock-driven engine
 *        components (extracted at the rule-of-three, quality bar §3).
 *
 * A plain `nowMs >= deadlineMs` breaks across the ~49.7-day millis() wrap — which for an endlessly
 * running appliance is normal operation, not a corner case. At the wrap a direct comparison cuts a
 * window short or, worse, collapses a quiesce settle to zero and reopens the sink-reset race §4
 * invariant #11 closes, and fires a drain or backoff at the wrong moment. The signed difference is
 * correct while the true interval stays under 2^31 ms (~24.8 days), which every window this appliance
 * schedules is (hunt dwell, drain ceiling, backoff, settle, sync cadence, LED heartbeat). Do not
 * "simplify" this back to a direct comparison.
 *
 * This began as three self-contained copies (HuntEngine, UploadSupervisor, and the LED status
 * surface); the third caller is where the rule of three says to extract, so it lives here — one
 * canonical definition all clock-driven components share (ADR-0013 established the discipline for
 * ChannelHopper::tick).
 */
#pragma once

#include <cstdint>

namespace sapper {

/// True once @p nowMs has reached @p deadlineMs, correct across the millis() wrap for intervals under
/// ~24.8 days. Pure and header-only so every clock-driven unit uses the one implementation.
inline bool reached(uint32_t nowMs, uint32_t deadlineMs) {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

}  // namespace sapper
