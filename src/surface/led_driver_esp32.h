/**
 * @file led_driver_esp32.h
 * @brief The on-device status-LED driver behind the LedDriver seam (ADR-0021, ADR-0002). Device-only.
 *
 * Renders a semantic LedStatus onto the active board's status indicator: an RGB WS2812 gets a colour,
 * a single LED gets on/off, a board with no LED renders nothing on hardware. Whatever the hardware,
 * every status transition is also logged to Serial as `[LED] status=<name>` — that line is the
 * machine-checkable half of the on-air verify (the physical colour is the human half). All board
 * facts (LED kind, GPIO) come from the Board Profile (ADR-0002), so this driver hardcodes no pin.
 */
#pragma once

#ifndef UNIT_TEST

#include "config/board_profile.h"
#include "surface/led_driver.h"

namespace sapper {

/// Drives the status LED described by @p profile. Construct with the active board profile; the driver
/// reads its LedKind and ledPin and nothing else about the board.
class Esp32LedDriver : public LedDriver {
public:
    explicit Esp32LedDriver(const BoardProfile& profile) : profile_(profile) {}

    /// Configure the GPIO. Call once from setup-time wiring, after the Arduino core is up — never from a
    /// file-scope constructor, which runs before GPIO is initialised. A no-op for the RGB path
    /// (rgbLedWrite drives the RMT directly) and the headless path.
    void begin();

    void show(LedStatus status) override;

private:
    const BoardProfile& profile_;
    /// The last non-Off status logged, so the heartbeat's per-second Off/on blink toggles do not spam
    /// Serial: the physical LED still blinks every call, but the diagnostic line is one per semantic
    /// change (hunting → working → …), not one per second.
    LedStatus lastLogged_ = LedStatus::Off;
};

}  // namespace sapper

#endif  // UNIT_TEST
