---
id: '0084'
title: "Direct-to-panel display on a no-PSRAM classic ESP32 (CYD): no full-frame canvas"
type: architecture
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Context

slice-0085 lights the ESP32-3248S035C ("CYD") ST7796 320x480 panel. The CYD is the no-PSRAM classic
ESP32 that ADR-0082 brought up headless; slice-0083 shipped the hunter and deferred the panel to this
slice precisely because the display HAL does not fit this board.

The display HAL (`LgfxDisplay`, ADR-0002) renders every surface into a **full-screen `LGFX_Sprite`**
(RGB565) and `present()` pushes the whole sprite to the panel (`lgfx_display.cpp`: `createSprite(w, h)`
then `pushSprite(0, 0)`). All three existing boards fit that model:

| Board | Panel | Full-frame sprite |
|---|---|---|
| Cardputer / M5StickC Plus2 | 240x135 | 64800 B |
| T-embed | 320x170 | 108800 B (octal PSRAM) |
| **CYD** | **320x480** | **307200 B** |

**Measured.** A 320x480 RGB565 canvas is `320 * 480 * 2 = 307200 B`. The CYD's measured largest free
block at the Provisioning stage is **110580 B** (ADR-0082: `heap_free=245888 heap_max=110580`). A
`createSprite(320, 480)` cannot succeed: the request is ~2.8x the largest contiguous block. Even a
half-frame `320x160` sprite is `102400 B`, which leaves only ~8 KB of headroom in the largest block and
competes directly with the TLS handshake buffers a sync allocates from the same internal DRAM pool. The
full-frame sprite HAL is infeasible on this board, and there is no PSRAM to place the canvas in (the CYD's
defining constraint, ADR-0082).

The content this board must draw is a text status HUD, the boot splash (ADR-0041), and the Maintenance
connection panel (ADR-0039): static chrome plus a handful of text fields that update on state change. No
animation, no photographic blit. That is what makes a buffer-free renderer adequate here where it would
not be for a game or a video surface.

## Decision

1. **On a board that cannot afford a full-frame sprite, `IDisplay` is backed by the LovyanGFX panel
   directly, with no offscreen canvas.** `fillScreen`/`drawPixel`/`drawText` write straight to the ST7796
   over SPI; `present()` is a no-op (there is nothing to push). This is a new `DirectPanelDisplay` class
   device-side, the sibling of `LgfxDisplay`, both implementing the same `IDisplay` seam so every surface
   is unchanged.

2. **The renderer is selected at compile time by a capability flag, `-DSAPPER_DISPLAY_DIRECT_PANEL`, set
   in the env** alongside `board`/`flash_size`/`BOARD_HAS_PSRAM`/`SAPPER_MANIFEST_HEAP_INTERNAL` (the same
   build-time capability layer, ADR-0002). The flag names the **constraint**, not the chip: "this board's
   panel is too large for a full-frame RGB565 sprite in the available RAM, and it has no PSRAM to escape
   to." A board with the flag instantiates `DirectPanelDisplay`; a board without it keeps the sprite-backed
   `LgfxDisplay`, byte-for-byte as the three existing boards ship it. This mirrors ADR-0082's choice to
   name the manifest-placement constraint in a flag rather than branch on the board macro.

3. **`readCanvas`/`canvasByteLength` return 0 (there is no canvas), so the panel-parameter proof is
   photographic, not a serial dump.** The `IDisplay` contract already permits this: both return 0 "when
   there is no canvas" (the `NullDisplay` path). ADR-0002 §5 and ADR-0041 already establish the
   panel-parameter proof (colour/byte-order, offset/clipping, rotation/mirror) as a human-reviewed
   artifact; on this board that review is a photograph of the real panel rather than a dumped canvas PNG.
   No verify the CYD previously had is lost, because the CYD had no panel at all before this slice. The
   serial `dump` read path (ADR-0045: it reads, never draws) is a no-op here exactly as on a `NullDisplay`.

4. **Flicker is controlled by not clearing the whole panel each frame, not by double-buffering.** The
   whole-frame sprite got flicker-free redraws for free from the single `pushSprite`; dropping the buffer
   means a surface must not clear the whole panel per frame. The surfaces this slice lights are **drawn
   once per entry**: the boot splash and the Maintenance connection panel each `fillScreen` + draw +
   `present()` a single time, so no per-frame clear arises and they show without flicker on the direct
   renderer. The **continuously-refreshing live hunt HUD is a separate case**: the shared
   `Esp32ScreenRenderer` it uses is the sprite-era renderer (it `fillScreen`s and redraws every frame and is
   laid out for 240x135), whose per-heartbeat redraw on a 320x480 direct panel would flash the whole panel
   about once a second. So it is **not wired on a direct-panel board in this slice** (`hunt_loop` gates it
   off); converting it to region-repaint (static chrome drawn once; a changed field repainted with an opaque
   background over its own rect, never a whole-panel clear) and re-laying it out for 480x320 landscape is a follow-on
   slice, best tuned on the physical panel (ADR-0002 §5; LEDGER). That region-repaint is the KISS
   alternative to double-buffering for a refreshing surface; an animated or image surface would instead need
   the banded display-list (slice-0085 Fork B), deliberately **not** built.

5. **The fail-loud init path is unchanged (ADR-0045).** `DirectPanelDisplay::begin()` returns the real
   panel init result; a genuine init failure routes every surface to the `NullDisplay` through
   `panelAfterInit`, logs `[FATAL] display init failed` once, and the headless hunt runs on.
   `DirectPanelDisplay` is simply the "real" arm of `panelAfterInit` on this board. A buffer-free renderer
   does not change what a *failed* panel does, only how a *working* one draws.

## Consequences

- A new `DirectPanelDisplay` (device-only, `#ifndef UNIT_TEST`, like `LgfxDisplay`) and a new ST7796
  LovyanGFX panel config for the CYD. The HAL panel selection in `lgfx_display.h` grows a CYD case guarded
  by `SAPPER_BOARD_HAS_DISPLAY`; the `IDisplay` instantiation site chooses `DirectPanelDisplay` when
  `SAPPER_DISPLAY_DIRECT_PANEL` is set, else `LgfxDisplay`.
- A new §4 standing invariant (#38) records the selection rule so a future reader does not "restore" a
  `createSprite` on a board that cannot afford one, nor collapse the two renderers into one.
- The CYD board profile flips `hasDisplay = true`, sets its geometry/colour-depth fields, and defines
  `SAPPER_BOARD_HAS_DISPLAY` so the panel config compiles. Per ADR-0002 §5 those pins and parameters are
  **confirmed by a panel probe on hardware before they are committed**, not guessed from a retailer pinout;
  until the probe confirms them the profile stays headless and the config holds documented candidate
  values. That confirmation is slice-0085's on-device step, the operator's to run (like the Stick's
  deferred PSRAM boot proof).
- GT911 capacitive touch is **not** part of this decision. This ADR is the renderer; touch is an input
  seam (`InputKind`) with no consumer yet (Maintenance entry already works via the BOOT-hold, ADR-0078),
  so slice-0085 defers it rather than wire a speculative input path (§3). Its candidate I2C pins are
  recorded in the LEDGER for the slice that gives touch a job.
- Extends ADR-0002 (build-time capability resolution) and ADR-0082 (the no-PSRAM classic ESP32 context),
  and leaves ADR-0045 (fail-loud init) and ADR-0041 (splash / photographic panel proof) in force
  unchanged. Supersedes none.
