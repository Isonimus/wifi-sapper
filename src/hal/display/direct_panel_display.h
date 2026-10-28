/**
 * @file direct_panel_display.h
 * @brief IDisplay backed by the LovyanGFX panel directly, with no offscreen canvas (ADR-0084).
 *
 * Device-only: pulls LovyanGFX and is compiled only when the active board has a display and selects the
 * direct renderer (SAPPER_BOARD_HAS_DISPLAY && SAPPER_DISPLAY_DIRECT_PANEL, ADR-0002 #4 / ADR-0084). The
 * sibling of LgfxDisplay for a board whose panel is too large for a full-frame RGB565 sprite in the
 * available RAM and has no PSRAM to place it in: on the CYD a 320x480 canvas is 307200 B against a measured
 * 110580 B largest free block (ADR-0082), so there is no `createSprite`. Every primitive writes straight to
 * the ST7796 over SPI; present() is a no-op (there is nothing to push), and readCanvas/canvasByteLength
 * return 0 (there is no canvas), so the panel-parameter proof is photographic, not a serial dump (ADR-0084
 * decision 3, ADR-0002 §5). Fail-loud init is unchanged (ADR-0045): begin() returns the real panel init
 * result and panelAfterInit routes a failed panel to the NullDisplay.
 */
#pragma once

#include "config/active_board.h"   // SAPPER_BOARD_HAS_DISPLAY
#include "config/board_profile.h"
#include "hal/display/display_hal.h"

#if SAPPER_BOARD_HAS_DISPLAY

#include "hal/display/lgfx_panel_select.h"   // ActiveLgfxPanel — shared with LgfxDisplay

namespace sapper {

class DirectPanelDisplay final : public IDisplay {
public:
    explicit DirectPanelDisplay(const BoardProfile& profile);

    bool begin() override;
    uint16_t width() const override { return m_width; }
    uint16_t height() const override { return m_height; }
    void fillScreen(uint16_t color565) override;
    void drawPixel(int32_t x, int32_t y, uint16_t color565) override;
    void drawText(int32_t x, int32_t y, const char* text, uint16_t color565, uint8_t size) override;
    void present() override;                               // no-op: no canvas to push
    void setBrightness(uint8_t level) override;
    size_t canvasByteLength() const override { return 0; } // no canvas (ADR-0084 decision 3)
    size_t readCanvas(size_t, uint8_t*, size_t) const override { return 0; }

private:
    const BoardProfile& m_profile;
    ActiveLgfxPanel m_panel;
    uint16_t m_width = 0;
    uint16_t m_height = 0;
};

}  // namespace sapper

#endif  // SAPPER_BOARD_HAS_DISPLAY
