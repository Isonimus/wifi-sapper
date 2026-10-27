/**
 * @file direct_panel_display.cpp
 * @brief Buffer-free IDisplay implementation: draws straight to the LovyanGFX panel (ADR-0084).
 */
#include "hal/display/direct_panel_display.h"

// Compiles to nothing on a screenless board (SAPPER_BOARD_HAS_DISPLAY undefined): the header pulls no
// LovyanGFX class there, so this impl has nothing to build. On a display board that uses the sprite-backed
// LgfxDisplay this class is simply unused.
#if SAPPER_BOARD_HAS_DISPLAY

namespace sapper {

DirectPanelDisplay::DirectPanelDisplay(const BoardProfile& profile)
    : m_profile(profile),
      m_width(profile.display.width),
      m_height(profile.display.height) {}

bool DirectPanelDisplay::begin() {
    // No canvas to allocate (ADR-0084): a 320x480 RGB565 sprite would be 307200 B against this board's
    // ~110 KB largest free block (ADR-0082). The only failure mode is the real panel init, which is the
    // loud-then-safe path (ADR-0045): a false return routes every surface to the NullDisplay.
    if (!m_panel.init()) return false;
    m_panel.setRotation(m_profile.display.rotation);
    // Without this the PWM backlight duty stays at 0 and the panel is black even though it initialised
    // (the failure slice-0005's first bring-up hit; the same on every board).
    m_panel.setBrightness(255);
    return true;
}

void DirectPanelDisplay::fillScreen(uint16_t color565) { m_panel.fillScreen(color565); }

void DirectPanelDisplay::drawPixel(int32_t x, int32_t y, uint16_t color565) {
    m_panel.drawPixel(x, y, color565);
}

void DirectPanelDisplay::drawText(int32_t x, int32_t y, const char* text, uint16_t color565, uint8_t size) {
    // Top-left datum so (x, y) is the glyph box corner the renderer lays out against; an opaque background
    // is the caller's fillRect (the same contract as LgfxDisplay), which on this buffer-free path clears
    // only the field's own rect rather than the whole panel — the flicker control of ADR-0084 decision 4.
    m_panel.setTextDatum(textdatum_t::top_left);
    m_panel.setTextColor(color565);
    m_panel.setTextSize(size);
    m_panel.drawString(text, x, y);
}

// Nothing to push: every primitive already wrote to the panel over SPI (ADR-0084). present() stays in the
// IDisplay contract so a surface that draws-then-presents is unchanged; here it is a deliberate no-op.
void DirectPanelDisplay::present() {}

}  // namespace sapper

#endif  // SAPPER_BOARD_HAS_DISPLAY
