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
    int8_t lcdCs, lcdDc, lcdRst, lcdBl;
    int8_t sdCs, sdMosi, sdMiso, sdClk;
};

}  // namespace sapper
