/**
 * @file led_driver.h
 * @brief The status-LED HAL seam and its semantic status vocabulary (ADR-0021, ADR-0001).
 *
 * The seam that keeps the LED *policy* (which fact lights which status, and the heartbeat timing)
 * pure and host-testable, and the LED *hardware* (a WS2812 on a GPIO, a plain LED, a serial line)
 * behind an abstract class — the same pure-core/HAL-seam split the capture and station paths use
 * (ADR-0009/ADR-0015). The driver is told a *semantic status*, not a colour: the surface decides the
 * status from engine facts (led_status_surface.h), and the driver renders it however its hardware
 * can (an RGB LED picks a colour, a single LED picks on/off, the serial driver logs it). So the
 * interesting logic — event → status, and the alive-heartbeat timing — is tested once on the native
 * lane against a fake driver, and porting to a new board is a new LedDriver, not new policy.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// What the status indicator is telling the operator. The surface maps engine facts onto these; a
/// driver renders each as best its hardware allows. Ordered from calmest to most urgent.
enum class LedStatus : uint8_t {
    Off,        ///< Dark — not begun, or between heartbeat blinks.
    Hunting,    ///< Normal steady state: promiscuous hunting, nothing wrong (rendered calm/green).
    Working,    ///< An STA window is open — off-air, uploading/syncing (rendered busy/blue).
    Degraded,   ///< The last cycle could not reach the network (did not associate) — uploads and the
                ///< hourly sync are stalled until connectivity returns (rendered warning/amber).
    Fault,      ///< A hard fault: the engine could not resume promiscuous mode (rendered alarm/red, solid).
    Recovered,  ///< A new password was just recovered — a latched celebratory flash (rendered white).
};

/**
 * @brief Abstract status-LED sink. show() is called only when the displayed status changes (edge,
 * not level), so a driver may treat each call as a discrete state transition and need not debounce.
 */
class LedDriver {
public:
    virtual ~LedDriver() = default;

    /// Render @p status now. Called from the app task, never a driver-callback context.
    virtual void show(LedStatus status) = 0;
};

}  // namespace sapper
