/**
 * @file tca8418_event.h
 * @brief Pure decode of a TCA8418 key-event FIFO byte + the Maintenance-key predicate (ADR-0057).
 *
 * The Cardputer ADV reads its keyboard through a TCA8418 I2C keypad-scan controller (there is no
 * runtime-pressable GPIO0 button — GPIO0 is a boot strap only, which is why the ADR-0053 windowed
 * GPIO0 poll was unreachable in the field). Each key event the controller queues is a single FIFO
 * byte; this header is the *decode* of that byte and the "is this the Maintenance key?" predicate,
 * with no I2C. It is host-testable (native lane), the pure half of the ADR-0057 pure/impure split;
 * the device driver that reads bytes off the wire lives in `tca8418_keyboard.{h,cpp}` (device-only).
 *
 * TCA8418 event-byte layout (datasheet KEY_EVENT_A): bit7 is the press/release state (1 = press,
 * 0 = release) and the low 7 bits are the 1-based key number (1..80), 0 for the empty event. The
 * electrical position is the 0-based key number: `row = (keyNumber-1)/10`, `col = (keyNumber-1)%10`.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// The raw 1-based TCA8418 key number of the `m` key ("m" for Maintenance) on the ADV keymap. Derived
/// from M5's ADV key table and *confirmed on-device* by the keyboard probe (`verify:keyboard`), which
/// logs the decoded key number of each press — the physical key→number correspondence is a measured
/// fact, so this constant records the measurement, not a guess (ADR-0057 decision 5). If the probe
/// reports a different number, amend ADR-0057 and this value together.
inline constexpr uint8_t kMaintenanceKeycode = 48;

/// A decoded TCA8418 key event. `keycode` is the raw 1-based key number (the FIFO byte's low 7 bits);
/// `row`/`col` are its 0-based electrical position, meaningful only for a real key (`keycode != 0`).
struct KeyEvent {
    bool pressed;      ///< FIFO bit7: true = key pressed, false = key released.
    uint8_t keycode;   ///< Raw 1-based key number (1..80); 0 for the empty event (`0x00`).
    uint8_t row;       ///< Electrical row = (keycode-1)/10 (undefined for keycode 0).
    uint8_t col;       ///< Electrical column = (keycode-1)%10 (undefined for keycode 0).
};

/// Split a raw TCA8418 FIFO byte into its press/release state and 1-based key number, deriving the
/// electrical row/col. Pure and total — every byte decodes, the empty event `0x00` giving keycode 0.
constexpr KeyEvent decodeKeyEvent(uint8_t rawEventByte) {
    const uint8_t keycode = static_cast<uint8_t>(rawEventByte & 0x7F);
    // row/col are derived from the 0-based position; only ever read for a real key (keycode != 0), so
    // the keycode-0 case (which would underflow the subtraction) is left as don't-care, not special-cased.
    const uint8_t electrical = static_cast<uint8_t>(keycode - 1);
    return KeyEvent{
        (rawEventByte & 0x80) != 0,
        keycode,
        static_cast<uint8_t>(electrical / 10),
        static_cast<uint8_t>(electrical % 10),
    };
}

/// Whether @p rawEventByte is a *press* of the Maintenance key. True only for a press (bit7 set) of
/// key number `kMaintenanceKeycode` — a release of that key, any other key, and the empty event all
/// return false, so a key-up never triggers entry and no other key is mistaken for it (ADR-0057).
constexpr bool isMaintenanceKey(uint8_t rawEventByte) {
    const KeyEvent event = decodeKeyEvent(rawEventByte);
    return event.pressed && event.keycode == kMaintenanceKeycode;
}

}  // namespace sapper
