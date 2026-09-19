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

#include "config/board_profile.h"
#include "hal/display/display_hal.h"
#include "hal/display/lgfx_cardputer.h"

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
    LGFX_Cardputer m_panel;
    lgfx::LGFX_Sprite m_canvas{&m_panel};
    uint16_t m_width = 0;
    uint16_t m_height = 0;
};

}  // namespace sapper
