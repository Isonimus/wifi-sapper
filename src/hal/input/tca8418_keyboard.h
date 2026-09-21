/**
 * @file tca8418_keyboard.h
 * @brief Minimal, purpose-built TCA8418 keyboard driver for runtime Maintenance entry (ADR-0057).
 *
 * Device-only glue (the impure half of ADR-0057's split; the pure decode is `tca8418_event.h`). It
 * does *only* what Maintenance entry — and confirming the entry keycode — needs: bring the Cardputer
 * ADV's TCA8418 I2C keypad controller out of power-on sleep, and drain its key-event FIFO. It is not
 * the full keyboard bring-up (text entry stays deferred to slice-8) — deliberately not the
 * M5Cardputer library, which would drag in M5GFX and collide with this repo's LovyanGFX-only stack.
 *
 * Not the M5 stack, but M5's own register sequence: the init writes mirror `Adafruit_TCA8418`'s
 * begin()/matrix()/flush()/enableInterrupts(). A GPIO11 falling-edge ISR sets a flag so the FIFO is
 * drained only when the controller signals an event — no per-loop I2C on the hunt path.
 */
#pragma once

#ifndef UNIT_TEST  // device-only: touches Wire/GPIO/ISRs, never compiled on the native lane.

#include <cstddef>
#include <cstdint>

namespace sapper {

/**
 * @brief The ADV's TCA8418 keypad controller, scoped to key-event drain for Maintenance entry.
 *
 * One instance, owned by main.cpp for the hunt loop's lifetime. `begin()` is called once on entering
 * Ready; `maintenanceKeyPressed()` is polled from `loop()` and is cheap until the INT line fires.
 */
class Tca8418Keyboard {
  public:
    /// Bring up I2C and the controller (the ADR-0057 register sequence) and attach the INT ISR.
    /// @return false (logged loud by the caller) if the controller does not ACK on the bus — runtime
    ///         Maintenance entry is then unavailable, but hunting (the primary mission) continues.
    bool begin();

    /// Whether the Maintenance key was pressed since the last call. Returns false immediately unless the
    /// INT ISR flagged a pending event; only then does it drain the FIFO (which also consumes unrelated
    /// keys — irrelevant until slice-8 brings up text entry). Built on drainRawEvents() + the pure
    /// isMaintenanceKey(), so entry and the keycode-confirming probe share one FIFO-drain path.
    bool maintenanceKeyPressed();

    /// Drain up to @p max raw FIFO event bytes into @p out, returning the count. Empty (returns 0)
    /// unless the INT ISR flagged an event, in which case it reads the queued events and acks INT. The
    /// bytes are undecoded on purpose — the pure `tca8418_event.h` decode is the single decoder both the
    /// entry predicate and the `verify:keyboard` probe (which logs each keycode to confirm `m` = 48) use.
    size_t drainRawEvents(uint8_t* out, size_t max);

  private:
    bool ready_ = false;  ///< begin() succeeded; guards the drain from touching a dead bus.
};

}  // namespace sapper

#endif  // UNIT_TEST
