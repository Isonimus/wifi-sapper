#include "config/button_hold.h"

namespace sapper {

bool decideButtonHold(bool pressed, uint32_t nowMs, uint32_t holdMs, ButtonHoldState& state) {
    if (!pressed) {
        // Released: reset so a bounce or a short press never fires, and a later hold starts fresh.
        state.pressing = false;
        state.fired = false;
        return false;
    }
    if (!state.pressing) {
        // Rising edge: start the hold timer; never fire on the edge itself (elapsed is 0).
        state.pressing = true;
        state.fired = false;
        state.pressStartMs = nowMs;
    }
    // Unsigned subtraction stays correct across a millis() wrap for any hold shorter than the wrap period.
    if (!state.fired && (nowMs - state.pressStartMs) >= holdMs) {
        state.fired = true;  // one hold is one entry: do not re-fire while the button stays held.
        return true;
    }
    return false;
}

}  // namespace sapper
