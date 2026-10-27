---
id: '0086'
title: "Direct-panel live HUD: a second ScreenRenderer with region-repaint, the sprite renderer untouched"
type: architecture
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Context

slice-0085 lit the CYD ST7796 panel through the buffer-free `DirectPanelDisplay` (ADR-0084) and shows the
boot splash and the Maintenance connection panel, both **drawn once**. It deliberately left the **live hunt
HUD** gated off on a direct-panel board, because the shared device renderer does not fit this board. ADR-0084
decision 4 named the two reasons and deferred the fix to a follow-on slice; this ADR records how that fix is
structured. slice-0087 is the work-unit.

**The two problems, read from the code, not estimated.**

1. **Whole-panel clear each render = flicker on a buffer-free panel.** The screen surface renders only on a
   view change: `ScreenToastSurface::tick()` calls `renderer_.render(view)` only when the computed view
   differs from the last one shown (`screen_toast_surface.cpp`, the `view != lastShown_` guard). But the
   `heartbeat` field toggles at `kHeartbeatHalfPeriodMs` (~1 Hz full period), so a view change arrives ~1 Hz
   even when nothing else moves. `Esp32ScreenRenderer::render()` opens with `display_.fillScreen(kBlack)` then
   redraws every field (`screen_renderer_esp32.cpp`). On a **sprite** board that clears the *offscreen* sprite
   and the single `present()`/`pushSprite` makes the swap atomic: flicker-free. On a **direct** panel
   `fillScreen` writes black straight to the glass before the redraw, so the whole panel flashes black ~1 Hz.
   The whole-panel clear is the flicker source, and it cannot be fixed in the `IDisplay` layer without the
   full-frame buffer this board cannot afford (ADR-0084): it must be fixed in how the renderer draws.

2. **The layout is hardcoded for 240x135.** Every coordinate in `screen_renderer_esp32.cpp` (`kMargin`,
   `kTitleY`, the `240 - 2*kMargin` bar width, the `135 - kBannerY` banner height) targets the small panel.
   The CYD is 480x320 landscape (rotation 1, slice-0085), ~2x wider and ~2.4x taller, so the HUD would render
   cornered in the top-left.

**The constraint that shapes the decision.** ADR-0084 decision 4 already warned that converting the *shared*
`Esp32ScreenRenderer` to region-repaint "touches the sprite boards so they must be re-verified," and the
LEDGER follow-up item carried that cost explicitly ("re-verify it on the sprite boards too"). The three
sprite boards (Cardputer, M5StickC Plus2, T-embed) ship a proven, flicker-free HUD today. This repo's grain
is to keep shipping boards byte-for-byte when a new board's needs can be met without touching them
(ADR-0080/0082/0084 all do exactly this for memory and for the `IDisplay` renderer). Re-verifying the sprite
HUD is an on-air, photographic check (the renderer's correctness is "the on-air screenshot's job, not a unit
test's," `screen_renderer_esp32.h`), the slowest kind to run.

## Decision

1. **The live HUD on a direct-panel board is drawn by a second `ScreenRenderer`, `DirectPanelScreenRenderer`,
   not by converting `Esp32ScreenRenderer`.** Both implement the same `ScreenRenderer` seam (ADR-0025) and
   consume the same `ScreenView`, so the pure `ScreenToastSurface` policy and the `ScreenView` model are
   unchanged and `Esp32ScreenRenderer` ships **byte-for-byte**: the three sprite boards need **no** on-air
   re-verification, and the cost ADR-0084 decision 4 / the LEDGER anticipated is avoided rather than paid.
   This is the `ScreenRenderer` analogue of ADR-0084 / §4 #38's two `IDisplay` renderers: a board whose
   constraint the shipping path cannot meet gets a sibling, not a rewrite of the shipping one.

2. **The renderer is selected at compile time by the existing capability flag `SAPPER_DISPLAY_DIRECT_PANEL`**,
   through an `ActiveScreenRenderer` typedef (`surface/active_screen_renderer.h`), mirroring the
   `ActiveLgfxPanel` selection (`lgfx_panel_select.h`). `hunt_loop` constructs `ActiveScreenRenderer` and the
   gate that disabled the HUD on a direct-panel board (slice-0085's `kWireLiveHud = false`) is removed: the
   HUD is now wired on every display board, each board getting the renderer its panel can afford. The flag
   names the **constraint** ("no full-frame canvas, draw straight to the glass"), not the chip, exactly as it
   does for `DirectPanelDisplay`.

3. **Flicker is controlled by region-repaint, never a whole-panel clear (ADR-0084 decision 4).** The static
   chrome (the "WiFi Sapper" title, which never changes) is drawn **once** on the first render; thereafter a
   field is repainted only when it changed, by filling its own rect with the background colour and redrawing
   the text over it (the opaque-background repaint that also clears a now-shorter string). The renderer holds
   the previously-rendered `ScreenView` to decide, per field, what changed; this is the renderer's own
   pixel-state concern and needs no change to the `render(const ScreenView&)` seam (the surface's coarse
   whole-view `!=` is a different granularity for a different purpose). `present()` stays a no-op on this board
   (the writes already reached the glass, ADR-0084). The CRACKED/CAPTURED banner is given its **own strip**
   below the HUD in the 480x320 layout (room the small panel lacked), so arming it repaints only that strip
   and clearing it only repaints that strip back to background: no HUD field sits under the banner, so none
   must be redrawn when it clears.

4. **`DirectPanelScreenRenderer` is a device renderer, photographically verified, and duplicates the small
   status/palette/field-text semantics rather than sharing them (rule of three).** This repo's seam places the
   pure, host-tested layer at `ScreenToastSurface` (which computes the `ScreenView`); the device renderer
   (`ScreenView` -> pixels, including text formatting and colour) is verified by the on-air photograph, a line
   the repo drew on purpose (`screen_renderer_esp32.h`). The palette constants are already duplicated between
   `panel_probe` and `Esp32ScreenRenderer`, an accepted state. With two renderers this is the **second**
   instance of the `ScreenView`->glyph mapping, where the quality bar says duplicate rather than abstract
   (the wrong abstraction costs more than a duplication; global §3). Extracting a shared pure
   field->text/colour helper is deferred to a **third** device renderer (LEDGER), where the rule of three
   earns it. Not extracting now keeps `Esp32ScreenRenderer` genuinely untouched, which is the whole point of
   decision 1.

5. **The 480x320 layout's exact pixel positions are candidate values, photo-tuned on the physical panel
   (ADR-0002 §5).** Like the ST7796 panel parameters slice-0085 proved by photograph, the HUD's font sizes
   and field coordinates are named constants whose final values are confirmed eyes-on, because the direct
   renderer has no canvas to dump (ADR-0084 decision 3). The region-repaint *logic* (chrome-once, changed
   fields only, banner in its own strip) is what must be correct independent of the exact coordinates; the
   coordinates are a nudge, not an invariant.

## Consequences

- A new `DirectPanelScreenRenderer` (`surface/`) and an `ActiveScreenRenderer` selection header
  (`surface/active_screen_renderer.h`). `hunt_loop` includes the selection header instead of
  `screen_renderer_esp32.h` directly, constructs `ActiveScreenRenderer`, and drops the `kWireLiveHud` gate.
- A new §4 standing invariant (#39) records the selection rule and the boundary, so a future reader does not
  "DRY" the two `ScreenRenderer`s into one (which would re-introduce the whole-panel clear on the direct
  board, or the 240x135 layout on the big panel) and does not restore the whole-panel `fillScreen` on the
  direct renderer. This mirrors §4 #38's protection of the two `IDisplay` renderers.
- `Esp32ScreenRenderer`, `ScreenToastSurface`, `ScreenView`, `resolveDisplay`/`panelAfterInit`, and every
  native test are unchanged. The native suite stands by construction (the new renderer is device-side and the
  pure policy did not move).
- The HUD's correctness on the CYD is a photograph, the operator's to capture on a provisioned device (like
  the splash and Maintenance photos slice-0085 deferred, and the Stick/T-embed on-device proofs). The
  machine-checkable half is the display build (the renderer compiles and is selected, `.dram0.bss` does not
  overflow, no full-frame sprite is allocated) and the error-gate half of `scripts/0034-hunt-hud-verify.mjs`.
- Extends ADR-0084 (its decision 4 is now implemented), ADR-0025 (the `ScreenRenderer`/`ScreenView` seam),
  and ADR-0033 (the live-hunt snapshot the HUD renders). Supersedes none: ADR-0084 decision 4 deferred this
  work and this ADR carries it out as that decision described.
