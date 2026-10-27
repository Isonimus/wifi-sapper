/**
 * @file direct_panel_screen_renderer.h
 * @brief The device ScreenRenderer for a buffer-free direct panel: 480x320 layout + region-repaint (ADR-0086).
 *
 * The sibling of Esp32ScreenRenderer for a direct-panel board (DirectPanelDisplay, ADR-0084): a board whose
 * panel is too large for a full-frame RGB565 sprite and has no PSRAM, so every primitive writes straight to
 * the glass and there is no offscreen canvas to clear atomically. Esp32ScreenRenderer's whole-panel
 * fillScreen() each render is flicker-free only because the sprite is offscreen; on a direct panel it flashes
 * the whole screen ~1 Hz (the heartbeat cadence). So this renderer controls flicker by REGION-REPAINT: the
 * static chrome (title) is drawn once; thereafter only a field whose value changed is repainted over its own
 * rect with an opaque background (ADR-0086 decision 3 / ADR-0084 decision 4). It holds the previously-rendered
 * ScreenView to decide, per field, what changed — the renderer's own pixel-state concern, so the
 * ScreenRenderer seam is unchanged.
 *
 * Both renderers deliberately duplicate the small status/palette/line-format semantics rather than share them:
 * this is the second instance of the ScreenView->glyph mapping, where rule-of-three says duplicate not
 * abstract (§3; a shared pure helper is deferred to a third renderer, LEDGER / ADR-0086 decision 4). Like
 * Esp32ScreenRenderer it is a device renderer whose correctness is the on-air photograph, not a unit test
 * (the direct panel has no canvas to dump, ADR-0084 decision 3); the 480x320 coordinates are candidate values
 * photo-tuned on the panel (ADR-0002 §5, ADR-0086 decision 5).
 */
#pragma once

#include "hal/display/display_hal.h"
#include "surface/screen_renderer.h"
#include "surface/screen_view.h"

// Only defined on a board that selects the direct renderer (ADR-0086 / §4 #39); on every other board this TU
// compiles to nothing and ActiveScreenRenderer is Esp32ScreenRenderer, so the sprite boards stay byte-for-byte
// (mirrors direct_panel_display.h's SAPPER_BOARD_HAS_DISPLAY guard).
#if defined(SAPPER_DISPLAY_DIRECT_PANEL)

namespace sapper {

class DirectPanelScreenRenderer : public ScreenRenderer {
public:
    explicit DirectPanelScreenRenderer(IDisplay& display) : display_(display) {}

    void render(const ScreenView& view) override;

private:
    void drawChromeOnce();
    void drawStatus(const ScreenView& view);
    void drawCounters(const ScreenView& view);
    void drawSync(const ScreenView& view);
    void drawHuntLine(const ScreenView& view);
    void drawBanner(const ScreenView& view);
    /// Fill an axis-aligned rect via the pixel primitive (the seam has no fillRect, ADR-0025 decision 3). Used
    /// to clear a field's own rect before redrawing it — the opaque-background repaint that controls flicker
    /// and erases a now-shorter string, never a whole-panel clear (ADR-0086 decision 3).
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565);

    IDisplay& display_;
    ScreenView last_{};        ///< The last view rendered; a field repaints only when it differs from this.
    bool chromeDrawn_ = false; ///< The static title is drawn once, on the first render.
};

}  // namespace sapper

#endif  // SAPPER_DISPLAY_DIRECT_PANEL
