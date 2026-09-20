/**
 * @file display_hal.h
 * @brief Board-agnostic display surface and its capability-based resolution (ADR-0002).
 *
 * The engine never touches this; a display is an event-bus observer (ADR-0001). A board with
 * `hasDisplay = false` resolves to the null implementation and compiles no `LGFX_*` class.
 * The resolution is a pure function of capability so it is unit-tested natively without
 * instantiating LovyanGFX.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "config/board_profile.h"

namespace sapper {

/// Which implementation a board's display HAL resolves to.
enum class DisplayResolution : uint8_t { Null, Real };

/// Pure capability query — `Real` when the board has a panel, else `Null` (ADR-0002 #4).
constexpr DisplayResolution resolveDisplay(const BoardProfile& profile) {
    return profile.hasDisplay ? DisplayResolution::Real : DisplayResolution::Null;
}

/**
 * @brief The minimal surface the bring-up frame and the serial `dump` need.
 *
 * RGB565 throughout; the canvas colour depth is a board fact (BoardProfile::display). Kept
 * deliberately small — it grows only when a surface needs more (KISS).
 */
class IDisplay {
public:
    virtual ~IDisplay() = default;

    /// Initialise the panel and canvas. Returns false only on a real init failure; a null
    /// display returns true, because "no panel present" is not a failure (ADR-0001).
    virtual bool begin() = 0;

    virtual uint16_t width() const = 0;
    virtual uint16_t height() const = 0;

    virtual void fillScreen(uint16_t color565) = 0;
    virtual void drawPixel(int32_t x, int32_t y, uint16_t color565) = 0;

    /// Draw NUL-terminated @p text with its top-left at (@p x, @p y), in @p color565, scaled by the
    /// integer @p size (1 = the base font). The one text primitive the status-HUD surface needs
    /// (ADR-0025 decision 3) — the seam grows only when a surface needs more (KISS). A null display
    /// no-ops it.
    virtual void drawText(int32_t x, int32_t y, const char* text, uint16_t color565, uint8_t size) = 0;

    /// Push the canvas to the physical panel.
    virtual void present() = 0;

    /// Total bytes in the RGB565 canvas (width*height*2), or 0 when there is none.
    virtual size_t canvasByteLength() const = 0;

    /// Copy up to @p cap canvas bytes starting at @p offset into @p out, for the verify dump.
    /// Returns bytes copied (0 at or past the end). Offset-based so the channel can stream the
    /// whole canvas through a small buffer without a 64 KB temporary (ADR-0003 bounded I/O).
    virtual size_t readCanvas(size_t offset, uint8_t* out, size_t cap) const = 0;
};

/// The panel a surface renders to after init: @p real when the panel initialised (@p initOk), else
/// @p fallback — a `NullDisplay`. On a real init failure a surface must not half-draw on the guarded
/// but dead LovyanGFX buffer (§3 fail-loud); routing it to the null fallback treats a *failed* panel
/// exactly as an *absent* one (the proven screenless path), so no new rendering path appears and the
/// headless hunt/serve runs on. The failure is announced loud at the init site; this only diverts the
/// draws. Pure (no LovyanGFX), so it is unit-tested natively like resolveDisplay (ADR-0045).
inline IDisplay& panelAfterInit(IDisplay& real, bool initOk, IDisplay& fallback) {
    return initOk ? real : fallback;
}

}  // namespace sapper
