/**
 * @file led_status_surface.h
 * @brief The status-LED surface: maps engine facts to LED status, with an alive heartbeat (ADR-0021).
 *
 * The first concrete surface on the event bus (ADR-0021, ADR-0001). It subscribes to the bus, keeps a
 * small state machine of "what is the appliance doing", and drives an abstract LedDriver so the whole
 * policy — which fact lights which status, and the timing of the alive-heartbeat blink and the
 * new-password flash — is pure and host-tested against a fake driver. It owns no hardware and no
 * clock: time enters only through tick(now), exactly like the engine and the supervisor, so the
 * blink cadence and the flash hold are asserted on the native lane with a fake clock.
 *
 * The heartbeat is the point of a status LED on a headless appliance: a *blinking* base colour proves
 * the firmware is still looping, where a solid colour could just be a hung device. So the normal
 * states (hunting, working, degraded) blink; only a hard Fault sits solid (an alarm, not a
 * heartbeat), and a freshly recovered password latches solid for a few seconds (a flash you cannot
 * miss) before returning to the heartbeat.
 */
#pragma once

#include <cstdint>

#include "core/event_bus.h"
#include "surface/led_driver.h"

namespace sapper {

/**
 * @brief Drives a status LED from bus events. Subscribe it to the bus, begin(now), tick(now) each loop.
 *
 * onAppEvent only updates internal state (it has no clock); tick(now) does every clock-based decision
 * and is the only thing that calls the driver — and it calls show() only when the displayed status
 * changes, so the driver sees discrete transitions, not a per-tick stream.
 */
class LedStatusSurface : public EventSink {
public:
    explicit LedStatusSurface(LedDriver& driver) : driver_(driver) {}

    /// Seed the clock anchor and light the LED in its initial hunting heartbeat. Call once after the
    /// engine is running, before the first tick().
    void begin(uint32_t nowMs);

    /// Bus subscription: update the base status (or arm the new-password flash) from one fact. Never
    /// touches the driver or reads a clock — tick() renders.
    void onAppEvent(const AppEvent& event) override;

    /// Advance the heartbeat/flash timing and render the current status. Call every app-loop iteration
    /// after the supervisor's tick, so a status change published this iteration shows the same iteration.
    void tick(uint32_t nowMs);

private:
    /// 1 s on + 1 s off = a 2 s heartbeat: slow enough to read as "alive", fast enough to notice it stop.
    static constexpr uint32_t kHeartbeatHalfPeriodMs = 1000;
    /// How long a newly recovered password holds the LED solid before it returns to the heartbeat.
    static constexpr uint32_t kRecoveredHoldMs = 5000;
    /// How long a fresh capture flash holds. Brief, and shorter than the recovered latch: captures are
    /// frequent, so a long hold would strobe the indicator and drown the heartbeat (ADR-0031 #4).
    static constexpr uint32_t kCapturedHoldMs = 800;

    /// Map a completed drain to its resting status: a hard resume fault is Fault; a cycle that associated
    /// is Hunting; a cycle that could not associate is Degraded. Degraded is scoped to connectivity — the
    /// one actionable "it can't reach the network" signal — and deliberately ignores per-capture upload
    /// outcomes (rejected, purged) and store errors, which are routine/rare, self-healing, and surfaced in
    /// the serial log and the later web dashboard rather than on this coarse indicator (slice-0022
    /// Scenario J on-air finding).
    static LedStatus statusFromDrain(const DrainOutcome& outcome);

    /// Whether the heartbeat is in its lit half at @p nowMs (wrap-safe against the millis() rollover).
    bool heartbeatOn(uint32_t nowMs) const;

    /// Compute the status to display now and push it to the driver only if it changed.
    void render(uint32_t nowMs);

    LedDriver& driver_;
    LedStatus baseStatus_ = LedStatus::Off;   ///< The underlying state the heartbeat blinks (or Fault, solid).
    LedStatus lastShown_ = LedStatus::Off;    ///< What the driver was last told, so show() is edge-only.
    uint32_t heartbeatAnchorMs_ = 0;          ///< When begin() ran; the blink phase is measured from here.
    bool recoveredPending_ = false;           ///< A NewPassword arrived; the next tick starts its hold.
    bool recoveredActive_ = false;            ///< The new-password flash is currently latched solid.
    uint32_t recoveredUntilMs_ = 0;           ///< When the flash latch releases back to the heartbeat.
    bool capturedPending_ = false;            ///< A HandshakeCaptured arrived; the next tick starts its flash.
    bool capturedActive_ = false;             ///< The capture flash is currently shown (outranked by Recovered).
    uint32_t capturedUntilMs_ = 0;            ///< When the capture flash releases back to the heartbeat.
    bool begun_ = false;
};

}  // namespace sapper
