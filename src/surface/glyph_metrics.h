/**
 * @file glyph_metrics.h
 * @brief The LovyanGFX default-font glyph box, shared across every surface that lays text out by hand.
 *
 * The default LovyanGFX font renders a ~6x8 px glyph box at scale 1, so a field drawn at integer `size`
 * is 6*size wide and 8*size tall. Both HUD renderers (screen_renderer_esp32.cpp, direct_panel_screen_renderer.cpp)
 * and the boot-face layout (boot_face_layout.h) position rows and version strings from these without a
 * textWidth() the IDisplay seam does not expose; the on-air photo stays the eyeball proof (ADR-0025).
 *
 * This consolidates the third duplication of the constants (slice-0104 added the boot-face site beside the
 * two renderers, tripping the rule of three, §3). Swapping each site's local constexpr for this include is
 * byte-identical at runtime, so ADR-0086 decision 1's "sprite boards ship with no on-air re-verification"
 * holds unchanged: only the definition site moved, not any value. Namespace-scope constexpr has internal
 * linkage, so this header is safe to include in every translation unit.
 */
#pragma once

#include <cstdint>

namespace sapper {

constexpr int32_t kGlyphW = 6;  ///< Default-font glyph box width at scale 1.
constexpr int32_t kGlyphH = 8;  ///< Default-font glyph box height at scale 1.

}  // namespace sapper
