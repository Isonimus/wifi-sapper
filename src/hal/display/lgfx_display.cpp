/**
 * @file lgfx_display.cpp
 * @brief LGFX_Sprite-backed IDisplay implementation (ADR-0002).
 */
#include "hal/display/lgfx_display.h"

#include <cstring>

namespace sapper {

LgfxDisplay::LgfxDisplay(const BoardProfile& profile)
    : m_profile(profile),
      m_width(profile.display.width),
      m_height(profile.display.height) {}

bool LgfxDisplay::begin() {
    if (!m_panel.init()) return false;
    m_panel.setRotation(m_profile.display.rotation);
    // Without this the PWM backlight duty stays at 0 and the panel is black even though it
    // initialised and the canvas is correct — the failure slice-0005's first bring-up hit.
    m_panel.setBrightness(255);

    m_canvas.setColorDepth(m_profile.display.colorBits);
    // A null buffer here means the SRAM budget could not fund the canvas — fail loud rather
    // than silently drawing nowhere (ADR-0001 no-PSRAM budget; global quality bar §3).
    if (m_canvas.createSprite(m_width, m_height) == nullptr) return false;
    return true;
}

void LgfxDisplay::fillScreen(uint16_t color565) { m_canvas.fillScreen(color565); }

void LgfxDisplay::drawPixel(int32_t x, int32_t y, uint16_t color565) {
    m_canvas.drawPixel(x, y, color565);
}

void LgfxDisplay::drawText(int32_t x, int32_t y, const char* text, uint16_t color565, uint8_t size) {
    // Top-left datum so (x, y) is the glyph box corner the renderer lays out against; opaque background
    // is left to the caller's fillScreen/fillRect, so a redraw does not leave stale pixels behind.
    m_canvas.setTextDatum(textdatum_t::top_left);
    m_canvas.setTextColor(color565);
    m_canvas.setTextSize(size);
    m_canvas.drawString(text, x, y);
}

void LgfxDisplay::present() { m_canvas.pushSprite(0, 0); }

size_t LgfxDisplay::canvasByteLength() const {
    // Honour the IDisplay contract: 0 when there is no canvas. If createSprite() failed in
    // begin() the geometry is still non-zero, so reporting m_width*m_height*2 here would make
    // streamDump() announce a payload it cannot send. Gate on the buffer, not the geometry.
    if (m_canvas.getBuffer() == nullptr) return 0;
    return static_cast<size_t>(m_width) * m_height * 2;  // RGB565: two bytes per pixel
}

size_t LgfxDisplay::readCanvas(size_t offset, uint8_t* out, size_t cap) const {
    const void* buffer = m_canvas.getBuffer();
    if (buffer == nullptr) return 0;
    const size_t byteLen = canvasByteLength();
    if (offset >= byteLen) return 0;
    const size_t remaining = byteLen - offset;
    const size_t n = remaining < cap ? remaining : cap;
    memcpy(out, static_cast<const uint8_t*>(buffer) + offset, n);
    return n;
}

}  // namespace sapper
