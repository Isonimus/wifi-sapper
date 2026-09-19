/**
 * @file screen_renderer.h
 * @brief The abstract output seam the screen surface draws through (ADR-0025 decision 2).
 *
 * The surface computes a ScreenView and hands it to a ScreenRenderer; the renderer turns it into
 * pixels. Keeping this a seam is what lets the pure policy be host-tested against a fake that merely
 * records the view — the device renderer (Esp32ScreenRenderer) is the only thing that touches the
 * panel, exactly as Esp32LedDriver is the only thing that touches the LED. Header-only and
 * hardware-free so it compiles on the native lane.
 */
#pragma once

#include "surface/screen_view.h"

namespace sapper {

/// Renders a view-model to some display. The surface calls render() only when the view changes.
class ScreenRenderer {
public:
    virtual ~ScreenRenderer() = default;

    /// Draw @p view. Called on the app task, only on a change, so an implementation may redraw the
    /// whole panel each call without a per-tick cost.
    virtual void render(const ScreenView& view) = 0;
};

}  // namespace sapper
