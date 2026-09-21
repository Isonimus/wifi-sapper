/**
 * @file entry_gesture.h
 * @brief Pure Maintenance-entry gesture logic (ADR-0053).
 *
 * Maintenance is entered by pressing the per-board entry button (`bootButtonPin`) during a bounded
 * window *after* a normal boot — never by holding it through reset. GPIO0/BOOT is the ESP32 strapping
 * pin: held LOW at reset-release it drops the ROM into serial download mode, so the firmware never
 * runs and the old "hold at power-on + single sample" gesture (ADR-0039 decision 2) could not work on
 * the reference Cardputer. Read *after* boot, GPIO0 is an ordinary input, so the escape is a
 * post-boot windowed poll.
 *
 * This header holds only the *decision* logic — the debounce state machine and its tuning constants —
 * so it is host-testable with no hardware. The millis-bounded poll loop that samples the real pin and
 * feeds this lives in `main.cpp` (device glue, review-only), like the single sample it replaces.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// How long the post-boot poll watches the entry button before giving up and booting normally. A UX
/// constant (ADR-0053), not a measurement: long enough to react to the splash, short enough not to
/// burden a rare boot on an endless-hunt appliance. An intentional press early-exits well under this.
inline constexpr uint32_t kMaintenanceEntryWindowMs = 3000;

/// Poll cadence within the window: one sample fed to the debouncer every this-many milliseconds.
inline constexpr uint32_t kEntryPollIntervalMs = 10;

/// Consecutive pressed (active-low) samples that confirm a real press. At the 10ms cadence this is a
/// ~20ms stable hold (three reads span two poll intervals) — long enough to reject an isolated
/// electrical glitch on the strapping pin, which
/// matters because a spurious entry exposes the recovered-PSK dashboard (ADR-0039 decision 5).
inline constexpr uint8_t kEntryDebounceSamples = 3;

/**
 * @brief Debounce state machine for the entry button: feed one active-low sample per poll.
 *
 * Returns true once `required` *consecutive* pressed (LOW) samples have been seen; a released (HIGH)
 * sample resets the run, so glitches and bounce never accumulate into a false confirmation. Pure and
 * allocation-free — one small counter — so it fits the no-PSRAM budget and is unit-testable off-device.
 */
class PressDebouncer {
  public:
    explicit constexpr PressDebouncer(uint8_t required) : required_(required), run_(0) {}

    /// @param pressed the current sample (true = button reads LOW = pressed).
    /// @return true the moment `required` consecutive pressed samples confirm a real press.
    constexpr bool feed(bool pressed) {
        run_ = pressed ? static_cast<uint8_t>(run_ + 1) : 0;
        return run_ >= required_;
    }

  private:
    uint8_t required_;
    uint8_t run_;
};

}  // namespace sapper
