/**
 * @file led_status_surface.cpp
 * @brief Implementation of the status-LED surface (ADR-0021).
 */
#include "surface/led_status_surface.h"

#include "core/deadline.h"          // reached(): wrap-safe latch/heartbeat timing.
#include "net/cracked_sync.h"       // SyncOutcome — a payload type the sink may read.
#include "net/upload_supervisor.h"  // DrainOutcome — the DrainCompleted payload statusFromDrain reads.

namespace sapper {

void LedStatusSurface::begin(uint32_t nowMs) {
    baseStatus_ = LedStatus::Hunting;
    lastShown_ = LedStatus::Off;
    heartbeatAnchorMs_ = nowMs;
    recoveredPending_ = false;
    recoveredActive_ = false;
    begun_ = true;
    render(nowMs);  // light the LED at boot rather than waiting for the first tick.
}

void LedStatusSurface::onAppEvent(const AppEvent& event) {
    switch (event.type) {
        case AppEventType::DrainStarted:
            baseStatus_ = LedStatus::Working;  // off-air, associating/uploading/syncing.
            break;
        case AppEventType::DrainCompleted:
            baseStatus_ = statusFromDrain(*event.drain);
            break;
        case AppEventType::NewPassword:
            recoveredPending_ = true;  // the next tick starts the solid flash (onAppEvent has no clock).
            break;
        case AppEventType::SyncCompleted:
            // No base change: the surrounding drain events set the state, and a genuinely new crack
            // arrives as its own NewPassword. A failed sync is deliberately not an LED alarm — it
            // retries silently, decoupled from the upload backoff (ADR-0021 / ADR-0019 decision #7).
            break;
        case AppEventType::FirstSyncSummary:
            // A fresh manifest seeds silently (ADR-0019 decision #5) — the backlog is not "recovered
            // now", so it raises no flash.
            break;
    }
}

void LedStatusSurface::tick(uint32_t nowMs) {
    if (!begun_) return;
    if (recoveredPending_) {
        recoveredPending_ = false;
        recoveredActive_ = true;
        recoveredUntilMs_ = nowMs + kRecoveredHoldMs;  // (re)arm the flash from now, extending on a repeat.
    }
    if (recoveredActive_ && reached(nowMs, recoveredUntilMs_)) recoveredActive_ = false;
    render(nowMs);
}

LedStatus LedStatusSurface::statusFromDrain(const DrainOutcome& outcome) {
    if (outcome.resumeFailed) return LedStatus::Fault;  // could not re-enter promiscuous — a hard fault.
    // Degraded is the operator's actionable connectivity signal: the last cycle could not associate, so
    // uploads and the hourly sync are stalled until the network is reachable. Per-capture upload outcomes
    // (a rejected handshake, a purged corrupt capture) and store errors are deliberately NOT degraded — a
    // rejected handshake is routine (partial captures, wpa-sec dedup) and retried, so painting this coarse
    // status amber on every rejection would make a healthy, online board read offline almost always
    // (observed on-air, slice-0022 Scenario J). Those details live in the serial log and, later, the web
    // dashboard, not on this one-colour indicator.
    return outcome.associated ? LedStatus::Hunting : LedStatus::Degraded;
}

bool LedStatusSurface::heartbeatOn(uint32_t nowMs) const {
    // uint32 subtraction gives the correct elapsed time across the millis() wrap; the lit/dark halves
    // alternate every kHeartbeatHalfPeriodMs.
    return ((nowMs - heartbeatAnchorMs_) / kHeartbeatHalfPeriodMs) % 2 == 0;
}

void LedStatusSurface::render(uint32_t nowMs) {
    LedStatus displayed;
    if (recoveredActive_) {
        displayed = LedStatus::Recovered;  // solid white flash overrides the base heartbeat.
    } else if (baseStatus_ == LedStatus::Fault) {
        displayed = LedStatus::Fault;      // an alarm sits solid, not blinking like a heartbeat.
    } else {
        displayed = heartbeatOn(nowMs) ? baseStatus_ : LedStatus::Off;  // blink the base colour: proof of life.
    }
    if (displayed != lastShown_) {
        driver_.show(displayed);
        lastShown_ = displayed;
    }
}

}  // namespace sapper
