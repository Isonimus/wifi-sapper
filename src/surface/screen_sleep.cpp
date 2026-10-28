/**
 * @file screen_sleep.cpp
 * @brief Implementation of the inactivity backlight dimmer (ADR-0093).
 */
#include "surface/screen_sleep.h"

namespace sapper {

ScreenSleepStage inactivityStage(uint32_t nowMs, uint32_t lastActivityMs, uint32_t dimAfterMs,
                                 uint32_t offAfterMs) {
    if (dimAfterMs == 0) return ScreenSleepStage::Active;  // feature disabled.
    const uint32_t elapsed = nowMs - lastActivityMs;       // wrap-safe over the millis() rollover.
    if (offAfterMs != 0 && elapsed >= offAfterMs) return ScreenSleepStage::Off;
    if (elapsed >= dimAfterMs) return ScreenSleepStage::Dimmed;
    return ScreenSleepStage::Active;
}

const char* screenSleepStageName(ScreenSleepStage stage) {
    switch (stage) {
        case ScreenSleepStage::Active: return "active";
        case ScreenSleepStage::Dimmed: return "dimmed";
        case ScreenSleepStage::Off: return "off";
    }
    return "active";
}

void ScreenSleepController::begin(uint32_t nowMs) {
    lastActivityMs_ = nowMs;
    activityPending_ = false;
    lastStage_ = ScreenSleepStage::Active;
    display_.setBrightness(kScreenFullBrightness);
    begun_ = true;
}

void ScreenSleepController::onAppEvent(const AppEvent& event) {
    // Only the toast-worthy facts count as activity (the same set that raises a banner, ADR-0031/#17):
    // a capture or a crack brightens the HUD. A routine SyncCompleted is excluded so an hourly sync does
    // not defeat the dimming. No clock here (like the other surfaces); tick() folds it in.
    if (event.type == AppEventType::HandshakeCaptured || event.type == AppEventType::NewPassword) {
        activityPending_ = true;
    }
}

void ScreenSleepController::noteInput(uint32_t nowMs) { lastActivityMs_ = nowMs; }

ScreenSleepStage ScreenSleepController::tick(uint32_t nowMs) {
    if (!begun_) return ScreenSleepStage::Active;
    if (activityPending_) {
        lastActivityMs_ = nowMs;
        activityPending_ = false;
    }
    const ScreenSleepStage stage = inactivityStage(nowMs, lastActivityMs_, dimAfterMs_, offAfterMs_);
    if (stage != lastStage_) {
        applyStage(stage);
        lastStage_ = stage;
    }
    return stage;
}

void ScreenSleepController::applyStage(ScreenSleepStage stage) {
    switch (stage) {
        case ScreenSleepStage::Active: display_.setBrightness(kScreenFullBrightness); break;
        case ScreenSleepStage::Dimmed: display_.setBrightness(kScreenDimBrightness); break;
        case ScreenSleepStage::Off: display_.setBrightness(0); break;
    }
}

}  // namespace sapper
