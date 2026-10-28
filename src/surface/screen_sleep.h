/**
 * @file screen_sleep.h
 * @brief Inactivity backlight dimming: a pure two-stage predicate + a bus-driven controller (ADR-0093).
 *
 * The battery/OPSEC win of a dark screen on an idle field unit, WITHOUT ever sleeping the CPU: the whole
 * mechanism is `IDisplay::setBrightness`, so the hunt loop keeps running in the dark (§4, ADR-0093 decision
 * 1). The stage is a pure function of elapsed idle time (`inactivityStage`), host-tested at its boundaries;
 * the controller is a bus observer like `LedStatusSurface`/`ScreenToastSurface` (ADR-0021/ADR-0025):
 * `onAppEvent` notes activity without a clock, `tick(now)` makes the clock-based decision and is the only
 * thing that drives the backlight. Activity is a notable bus fact (a capture or a crack), plus an input
 * wake where a board has one (`noteInput`), since the Sapper is headless-first and the HUD is event-driven,
 * not an interactive menu.
 */
#pragma once

#include <cstdint>

#include "core/event_bus.h"
#include "hal/display/display_hal.h"

namespace sapper {

/// The backlight stage for a given idle time. Ordered brightest to darkest.
enum class ScreenSleepStage : uint8_t { Active, Dimmed, Off };

/// Full backlight, and the readable dim level (~15% of full: most of the draw saved, counts still legible,
/// ADR-0093 decision 2). Architecture-derived like the Adversary's 15% band; tuning deferred (LEDGER).
constexpr uint8_t kScreenFullBrightness = 255;
constexpr uint8_t kScreenDimBrightness = 38;

/**
 * @brief The stage for an idle time, as a pure predicate (ADR-0093 decision 2).
 *
 * `dimAfterMs == 0` disables dimming (always Active). Otherwise the elapsed idle is `nowMs -
 * lastActivityMs` (wrap-safe over the millis() rollover): Off once `offAfterMs` is passed (when nonzero),
 * else Dimmed once `dimAfterMs` is passed, else Active. Off wins past its threshold even if a hand-edited
 * config set `offAfterMs < dimAfterMs`, so a bad config can never strand the screen on.
 */
ScreenSleepStage inactivityStage(uint32_t nowMs, uint32_t lastActivityMs, uint32_t dimAfterMs,
                                 uint32_t offAfterMs);

/// A short lower-case name for the serial `[DIM]` diagnostic line (the machine-checkable verify half).
const char* screenSleepStageName(ScreenSleepStage stage);

/**
 * @brief Drives the backlight from idle time + bus activity. Subscribe to the bus, begin(now), tick(now)
 * each hunt-loop iteration; call noteInput(now) from loop() on a board input.
 *
 * Holds an IDisplay by reference (the real panel on device, a fake that records setBrightness in the host
 * test), so the whole stage->brightness policy is asserted on the native lane. onAppEvent has no clock
 * (it only flags activity); tick() renders and calls setBrightness only on a stage change.
 */
class ScreenSleepController : public EventSink {
public:
    ScreenSleepController(IDisplay& display, uint32_t dimAfterMs, uint32_t offAfterMs)
        : display_(display), dimAfterMs_(dimAfterMs), offAfterMs_(offAfterMs) {}

    /// Light the panel full and seed the idle anchor. Call once after the engine is running.
    void begin(uint32_t nowMs);

    /// Bus subscription: note activity on a capture or a crack (the toast-worthy facts). A routine
    /// SyncCompleted is deliberately not activity, so an hourly sync does not keep the screen lit. No clock.
    void onAppEvent(const AppEvent& event) override;

    /// A board input (any key / the entry button pressed) resets the idle timer so the operator can wake
    /// the HUD by hand. A no-op path for a board with no pollable input.
    void noteInput(uint32_t nowMs);

    /// Fold any pending activity into the idle anchor, compute the stage, drive the backlight on a change,
    /// and return the current stage (so the device loop can log the transition).
    ScreenSleepStage tick(uint32_t nowMs);

    /// Whether dimming is enabled at all (dimAfter > 0). The device loop wires the controller only when so.
    bool enabled() const { return dimAfterMs_ != 0; }

private:
    void applyStage(ScreenSleepStage stage);

    IDisplay& display_;
    uint32_t dimAfterMs_;
    uint32_t offAfterMs_;
    uint32_t lastActivityMs_ = 0;
    bool activityPending_ = false;                        ///< A fact arrived; the next tick folds it in.
    ScreenSleepStage lastStage_ = ScreenSleepStage::Active;  ///< Last stage driven, so setBrightness is edge-only.
    bool begun_ = false;
};

}  // namespace sapper
