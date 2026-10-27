/**
 * @file active_screen_renderer.h
 * @brief Compile-time selection of the active board's device ScreenRenderer (ADR-0086).
 *
 * The status-HUD/toast surface (ScreenToastSurface) drives an abstract ScreenRenderer (ADR-0025); which
 * concrete renderer backs it is a board fact, selected here in one place on the same capability flag the
 * panel class (ActiveLgfxPanel, lgfx_panel_select.h) and the IDisplay backend (main.cpp display()) use.
 *
 * A direct-panel board (SAPPER_DISPLAY_DIRECT_PANEL, ADR-0084: panel too large for a full-frame sprite, no
 * PSRAM) gets DirectPanelScreenRenderer, which lays the HUD out for 480x320 and repaints only changed fields
 * (no whole-panel fillScreen per heartbeat, which would flash a buffer-free panel ~1 Hz). Every other display
 * board keeps the sprite-era Esp32ScreenRenderer, byte-for-byte: the full-frame sprite makes its whole-panel
 * clear flicker-free, so the three sprite boards are unchanged and need no re-verification (ADR-0086 #1 / §4
 * #39). Both implement the same ScreenRenderer seam and consume the same ScreenView, so the surface is one
 * code path. Header-only glue; it pulls only the chosen renderer's header.
 */
#pragma once

#if defined(SAPPER_DISPLAY_DIRECT_PANEL)
#include "surface/direct_panel_screen_renderer.h"
namespace sapper {
using ActiveScreenRenderer = DirectPanelScreenRenderer;
}
#else
#include "surface/screen_renderer_esp32.h"
namespace sapper {
using ActiveScreenRenderer = Esp32ScreenRenderer;
}
#endif
