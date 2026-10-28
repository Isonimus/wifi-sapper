/**
 * @file board_profile.h
 * @brief Compile-time board capability descriptor (ADR-0002).
 *
 * One `constexpr BoardProfile` per board replaces the Adversary's scattered `TARGET_*`
 * `#ifdef`s. Core code branches on what a board *has* (`profile.hasDisplay`) never on what
 * it *is*, so adding a board is one descriptor plus one `SAPPER_BOARD_*` flag, and a missing
 * capability is a compile-time absence rather than a silent runtime failure.
 *
 * The type is a plain aggregate of scalars: constexpr, no heap, no RTTI — it must fit the
 * no-PSRAM SRAM budget (ADR-0001), and a runtime-detected profile would be a new decision
 * (superseding ADR-0002), not an in-place change here.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// What the board reads user input through. `None` is a headless appliance (ADR-0001).
enum class InputKind : uint8_t { None, Keyboard, Buttons, Touch };

/// Where captures and config persist. `None` means RAM/NVS only.
enum class StorageKind : uint8_t { None, Sd, LittleFs };

/// The status indicator an alert surface can drive (slice-7).
enum class LedKind : uint8_t { None, Single, Rgb };

/// Panel dimensions and the canvas colour depth the memory policy tunes per board (ADR-0002).
struct DisplayGeometry {
    uint16_t width;
    uint16_t height;
    uint8_t rotation;    ///< LovyanGFX rotation index.
    uint8_t colorBits;   ///< Bits per canvas pixel; RGB565 = 16, palette = 8 (memory ADR).
};

/**
 * @brief The single source of board facts. Populated once per board; queried by capability.
 *
 * Pin fields carry the documented GPIO for peripherals later slices bring up (storage, LED,
 * battery). They are data in the one board-fact table ADR-0002 mandates, not per-slice
 * additions — a value of -1 means "absent on this board".
 */
struct BoardProfile {
    const char* name;

    // --- display -----------------------------------------------------------
    bool hasDisplay;
    DisplayGeometry display;   ///< Meaningful only when hasDisplay is true.

    // --- capabilities ------------------------------------------------------
    InputKind input;
    StorageKind storage;
    bool hasPsram;
    LedKind led;

    // --- pins (‑1 = absent) ------------------------------------------------
    int8_t ledPin;
    int8_t batteryAdcPin;
    /// The board's battery voltage-divider ratio in permille (2000 = x2.000, a 1:2 divider), used by the
    /// pure BatteryModel to scale the gauge's raw pin millivolts to cell millivolts (ADR-0091, §4 #40). A
    /// value of 0 means "divider not yet measured on this board": the model reports BatteryLevel::Unknown
    /// rather than a guessed charge (measure-don't-estimate), so a board with a battery pin but no measured
    /// ratio shows no level until its divider is measured (a one-line data change then, LEDGER). Carries a
    /// 0 default so a board profile that omits it fails safe to Unknown, never a wrong level.
    uint16_t batteryDividerPermille = 0;
    int8_t lcdCs, lcdDc, lcdRst, lcdBl;
    int8_t sdCs, sdMosi, sdMiso, sdClk;
    /// A per-board entry button that polls for Maintenance entry. Active-low (reads LOW when pressed).
    /// Superseded as the *reference-board* entry path by ADR-0057: the Cardputer ADV has no
    /// runtime-pressable button wired to GPIO0 — GPIO0 is a boot strap only (holding it at reset-release
    /// enters ROM download mode, and post-boot it reads a floating/high strap, never a press), so the ADV
    /// declares -1 and enters Maintenance by a runtime keyboard 'm' press that reboots. The field is the
    /// entry button for a board that genuinely wires a pollable non-strapping button (the M5StickC Plus2's
    /// BtnB side button, GPIO39); the per-board entry gesture is now a deliberate debounced *hold* of this
    /// pin, decided in ADR-0078 and selected at compile time by `input` (not board identity). -1 means "no
    /// pollable entry button." Unlike the peripheral pins this one *gates a mode*, so it carries a `-1`
    /// default member initializer: a future board profile that forgets it defaults to "absent" (no
    /// spurious Maintenance entry / PSK exposure), not to GPIO0 — the one pin where an accidental zero
    /// would be behavioural.
    int8_t bootButtonPin = -1;

    /// A software power-latch (keep-alive) pin some boards need asserted HIGH in `setup()` or they power
    /// off the moment the power button is released (the M5StickC Plus2's GPIO4). A board fact in the sense
    /// ADR-0002 defines ("the GPIO/bus pins the engine and peripherals need"); -1 = no latch to hold (the
    /// USB-powered Cardputer), so its boot is unchanged. Carries a -1 default like `bootButtonPin`.
    int8_t powerHoldPin = -1;
};

}  // namespace sapper
