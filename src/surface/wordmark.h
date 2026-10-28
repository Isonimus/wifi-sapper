/**
 * @file wordmark.h
 * @brief The product wordmark drawn with the banner's magenta drop shadow (slice-0097).
 */
#pragma once

#include <cstdint>

#include "hal/display/display_hal.h"

namespace sapper {

/**
 * @brief Draw @p text as the product wordmark: white glyphs over a banner-magenta drop shadow.
 *
 * The banner renders "WiFi Sapper" as white glyphs inside a magenta neon glow. The IDisplay seam has no
 * alpha or blur (one text primitive only, ADR-0025 decision 3), so a soft glow is not drawable; this
 * emulates it as a hard magenta drop shadow offset down-right behind the white glyphs. Two drawText calls;
 * white is drawn last so the readable glyphs sit on top of the shadow.
 *
 * Shared by all three wordmark sites (both HUD renderers and the boot splash, the rule-of-three extraction,
 * §3) so the effect is tuned in one place; it is a leaf drawing utility, not a merge of the renderers (§4
 * #39). @p size is the LovyanGFX integer glyph scale; the shadow offset scales with it so the effect reads
 * the same across panel tiers. Pure over the IDisplay seam, so it is unit-tested natively against a
 * recording fake (no LovyanGFX). A null/absent display no-ops every draw.
 */
void drawWordmark(IDisplay& display, int32_t x, int32_t y, const char* text, uint8_t size);

}  // namespace sapper
