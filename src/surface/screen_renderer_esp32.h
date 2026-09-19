/**
 * @file screen_renderer_esp32.h
 * @brief The device ScreenRenderer: lays a ScreenView out on the panel (ADR-0025).
 *
 * Holds the shared IDisplay (the one the bring-up frame and the serial dump use — one canvas, one
 * owner) and turns a view-model into glyphs: a persistent HUD, and a CRACKED banner over its lower
 * part when a toast is active. It is the only screen code that knows pixels, fonts, and colours, so
 * the surface policy can stay pure. Not compiled on the native lane (the fake renderer stands in
 * there); its correctness is the on-air screenshot's job (slice-0026 Scenario J), not a unit test's.
 */
#pragma once

#include "hal/display/display_hal.h"
#include "surface/screen_renderer.h"

namespace sapper {

class Esp32ScreenRenderer : public ScreenRenderer {
public:
    explicit Esp32ScreenRenderer(IDisplay& display) : display_(display) {}

    void render(const ScreenView& view) override;

private:
    void drawHud(const ScreenView& view);
    void drawBanner(const ScreenView& view);
    /// Fill an axis-aligned rect via the pixel primitive — the seam has no fillRect (it grows only when
    /// needed, ADR-0025 decision 3), and render() runs only on a change, so the cost is negligible.
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565);

    IDisplay& display_;
};

}  // namespace sapper
