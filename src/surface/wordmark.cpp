/**
 * @file wordmark.cpp
 * @brief Implementation of the product wordmark with a magenta drop shadow (slice-0097).
 */
#include "surface/wordmark.h"

namespace sapper {
namespace {

// The banner's wordmark-glow hue as a drop shadow, and the readable glyph colour on top. RGB565.
constexpr uint16_t kShadowMagenta = 0xF81F;  // offset down-right, behind the glyphs.
constexpr uint16_t kWordmarkWhite = 0xFFFF;  // the readable glyphs, drawn last so they sit on top.

// Drop-shadow offset in pixels: half the glyph-size scale, so it reads as a shadow rather than a double
// image and stays proportional across tiers (a size-3 title -> 1 px, size-4 -> 2 px). Floored at 1 so the
// shadow never vanishes. A candidate value photo-tuned on the panels (ADR-0002 §5): the full two-channel
// RGB split first tried here read too heavy on the 480x320 panel, so it was reduced to this single magenta
// shadow at half the offset. The white-over-magenta layering is the invariant; the offset is the nudge.
constexpr int32_t shadowOffset(uint8_t size) {
    const int32_t scaled = static_cast<int32_t>(size) / 2;
    return scaled < 1 ? 1 : scaled;
}

}  // namespace

void drawWordmark(IDisplay& display, int32_t x, int32_t y, const char* text, uint8_t size) {
    const int32_t off = shadowOffset(size);
    display.drawText(x + off, y + off, text, kShadowMagenta, size);  // magenta drop shadow, down-right
    display.drawText(x, y, text, kWordmarkWhite, size);             // white glyphs on top
}

}  // namespace sapper
