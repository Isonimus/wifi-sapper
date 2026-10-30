/**
 * @file boot_face_layout.h
 * @brief Resolution-aware layout tiers for the three boot faces (slice-0104).
 *
 * The splash (drawSplashFrame, ADR-0041), the Maintenance screen (drawMaintenanceScreen, ADR-0039),
 * and the first-boot provisioning screen (drawProvisioningScreen, ADR-0089) are a single IDisplay path
 * main.cpp drives on *every* panel, the CYD's DirectPanelDisplay included. Before this slice they were
 * fixed-coordinate, tuned for 240x135 and drawn top-left on a larger panel. This ports slice-0088's HUD
 * tier pattern to them, with THREE tiers instead of two because the boot faces serve three real sizes on
 * one code path (240x135 Cardputer/Stick, 320x170 T-embed, 480x320 CYD) where the HUD handled the CYD
 * with a separate renderer. Pure and header-only (like the ADR-0057 key-decode unit), so the tier
 * selection is host-tested on the native lane where main.cpp cannot be compiled.
 */
#pragma once

#include <cstdint>

namespace sapper {

// The default LovyanGFX font glyph box height at scale 1. Duplicated file-locally as kGlyphH in both HUD
// renderers (screen_renderer_esp32.cpp, direct_panel_screen_renderer.cpp); this is the THIRD site, which
// trips the rule of three. A shared glyph_metrics.h extraction is deferred (LEDGER) rather than scope-crept
// into this cosmetic slice and forced through the two frozen renderers; only the height is needed here.
constexpr int32_t kGlyphH = 8;

/**
 * @brief Shared layout primitives for a boot face, selected per panel size (slice-0104).
 *
 * A boot face is a title plus a short stack of text lines, so a tier needs only these four numbers; the
 * HUD's 20-field Layout (bars, banner, indicators) is deliberately NOT reused (slice-0104 Design). Each
 * face derives its own Y origin from these: the splash centres its title block from the panel height, the
 * info faces (Maintenance/provisioning) top-align the title at `margin`. A body line sits at
 * `bodyTop() + slot * lineGap`, and a face gaps a section by skipping a slot index (provisioning uses
 * slots 0,1,2,4,5). These are candidate values photo-tuned on the physical panels (ADR-0002 §5): the tier
 * *selection* by panel size is the invariant, the exact numbers are a nudge. A future reader must not
 * restore a hardcoded 240/135 here — this one path serves three panels at three resolutions.
 */
struct BootFaceLayout {
    int32_t margin;     ///< Left x for all text, and the top inset for the info faces' title.
    uint8_t titleSize;  ///< Glyph scale for the section title / splash wordmark.
    uint8_t bodySize;   ///< Glyph scale for body text lines.
    int32_t lineGap;    ///< Vertical advance between body line slots.

    /// The y of the first body line slot on an info face: below a top-aligned title plus one line gap.
    constexpr int32_t bodyTop() const { return margin + titleSize * kGlyphH + lineGap; }

    /// The bottom pixel of body slot @p slot, for the on-panel static_asserts and clip reasoning.
    constexpr int32_t slotBottom(int32_t slot) const {
        return bodyTop() + slot * lineGap + bodySize * kGlyphH;
    }
};

// Compact tier: 240x135 (Cardputer, M5StickC Plus2). Title size 3 so the splash wordmark keeps its
// pre-slice impact; body text size 1 (the only size whose longest line, "power-cycle to resume hunting"
// = 29 chars * 6 px = 174 px, fits the 240 px width). bodySize is bounded by width/(6*maxChars) at every
// tier, which is why the short 320 px roomy panel below also stays at size 1.
inline constexpr BootFaceLayout kCompact{/*margin*/ 4, /*titleSize*/ 3, /*bodySize*/ 1, /*lineGap*/ 15};

// Roomy tier: the LilyGo T-embed (320x170). Wider but short, so the gain over compact is a larger title
// (size 4 fits "SAPPER SETUP" = 12 * 24 = 288 px < 320) and airier line spacing, not bigger body text
// (the 29-char line still forces size 1 on a 320 px panel: 29 * 12 = 348 px would overflow).
inline constexpr BootFaceLayout kRoomy{/*margin*/ 8, /*titleSize*/ 4, /*bodySize*/ 1, /*lineGap*/ 18};

// Large tier: the CYD (480x320). The room to grow both the title (size 5, "SAPPER SETUP" = 360 px < 480)
// and the body text to size 2 (the 29-char line is 348 px < 480), with generous margins and spacing, so
// the boot face fills the panel instead of the old top-left 240x135 corner.
inline constexpr BootFaceLayout kLarge{/*margin*/ 16, /*titleSize*/ 5, /*bodySize*/ 2, /*lineGap*/ 34};

// A board gets the larger tier only when its panel is tall enough to lay the bigger fonts out without
// clipping; the branch is on panel capability, not board identity (ADR-0002). The 280 px large threshold
// sits above the T-embed's 170 px and at/below the CYD's 320 px, so the CYD gets `large` and the T-embed
// `roomy`; 160 px separates compact from roomy. The thresholds are photo-tuned candidates (slice-0088).
constexpr int32_t kRoomyMinHeight = 160;
constexpr int32_t kLargeMinHeight = 280;

// The tallest boot face is provisioning, whose lowest content is body slot 5 (slots 0,1,2,4,5, a skipped
// slot 3 for the section break). A nudge that pushes it off its design panel must fail the build, not the
// panel at runtime (mirrors both HUD renderers' banner-line static_assert).
constexpr int32_t kProvisioningLastSlot = 5;
static_assert(kCompact.slotBottom(kProvisioningLastSlot) <= 135,
              "compact tier's last provisioning line runs off the 240x135 panel");
static_assert(kRoomy.slotBottom(kProvisioningLastSlot) <= 170,
              "roomy tier's last provisioning line runs off the 320x170 panel");
static_assert(kLarge.slotBottom(kProvisioningLastSlot) <= 320,
              "large tier's last provisioning line runs off the 480x320 panel");

/// Select the boot-face layout tier for a panel of the given pixel @p height (slice-0104).
inline const BootFaceLayout& bootFaceLayoutFor(int32_t height) {
    if (height >= kLargeMinHeight) return kLarge;
    if (height >= kRoomyMinHeight) return kRoomy;
    return kCompact;
}

}  // namespace sapper
