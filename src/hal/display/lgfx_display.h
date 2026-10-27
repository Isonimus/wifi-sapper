/**
 * @file lgfx_display.h
 * @brief Real IDisplay backed by an LGFX_Sprite canvas over the board's LovyanGFX panel.
 *
 * Device-only: pulls LovyanGFX and is compiled only when the active board has a display
 * (ADR-0002 #4). All drawing lands on an off-screen `LGFX_Sprite` (RGB565), and `present()`
 * pushes it to the panel — the same canvas the serial `dump` reads back for the verify
 * artifact, so what a human reviews is exactly what was drawn.
 */
#pragma once

#include "config/active_board.h"   // SAPPER_BOARD_HAS_DISPLAY: a screenless board compiles no LGFX_* class (ADR-0002 #4)
#include "config/board_profile.h"
#include "hal/display/display_hal.h"

// A screenless board (hasDisplay=false, e.g. the headless CYD, ADR-0082) does NOT define
// SAPPER_BOARD_HAS_DISPLAY, so this whole translation unit (and lgfx_display.cpp) compiles to nothing and
// no LovyanGFX panel class is pulled — making the "A screenless board compiles no LGFX_* class" contract
// true at the file level, not just at main.cpp's include guard. A board that DOES declare a display but
// forgets its panel config still hits the hard #error below (the safety net is preserved).
#if SAPPER_BOARD_HAS_DISPLAY

#include "hal/display/lgfx_panel_select.h"   // ActiveLgfxPanel — the board's LovyanGFX config, shared with DirectPanelDisplay

namespace sapper {

class LgfxDisplay final : public IDisplay {
public:
    explicit LgfxDisplay(const BoardProfile& profile);

    bool begin() override;
    uint16_t width() const override { return m_width; }
    uint16_t height() const override { return m_height; }
    void fillScreen(uint16_t color565) override;
    void drawPixel(int32_t x, int32_t y, uint16_t color565) override;
    void drawText(int32_t x, int32_t y, const char* text, uint16_t color565, uint8_t size) override;
    void present() override;
    size_t canvasByteLength() const override;
    size_t readCanvas(size_t offset, uint8_t* out, size_t cap) const override;

private:
    const BoardProfile& m_profile;
    ActiveLgfxPanel m_panel;
    lgfx::LGFX_Sprite m_canvas{&m_panel};
    uint16_t m_width = 0;
    uint16_t m_height = 0;
};

}  // namespace sapper

#endif  // SAPPER_BOARD_HAS_DISPLAY
