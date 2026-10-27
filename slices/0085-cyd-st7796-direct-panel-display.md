---
id: '0085'
title: "CYD ST7796 320x480 display: direct-to-panel renderer"
type: slice
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Goal

Light the ESP32-3248S035C ("CYD") ST7796 320x480 panel as the board's display surface, via the
direct-to-panel renderer decided in ADR-0084. slice-0083 shipped the CYD headless (`hasDisplay = false`)
because the full-frame-sprite display HAL (`LgfxDisplay`, ADR-0002) would allocate a 307200 B
`createSprite` block (320x480x2), infeasible on this no-PSRAM classic ESP32 whose measured largest free
block is 110580 B (ADR-0082). This slice builds the buffer-free renderer (`DirectPanelDisplay`) so the
splash, the hunt HUD, and the Maintenance connection panel draw on the real panel, with the manifest still
in the internal DRAM heap (ADR-0082) unchanged.

This picks up the display half of the `[feature] CYD Slice B` ledger item. **GT911 capacitive touch is
deliberately deferred** (see Design): Maintenance entry already works via the BOOT-hold
(`InputKind::Buttons`, proven headless in slice-0083), so touch has no consumer yet and wiring a
speculative input path would violate the no-speculative-generality bar (quality-bar §3). The touch half is
re-logged as its own deferred ledger item with the candidate I2C pins.

## Definition of Done

- **Given** the CYD panel probe flashed with the candidate ST7796 pins (CS=15, DC=2, SCK=14, MOSI=13,
  MISO=12, BL=27; RST tied, documented pinout, to be confirmed)
- **When** `SAPPER_TEST_PANEL` renders the RGB-blocks/border/offset proof (ADR-0041)
- **Then** the pattern shows correct colour order, orientation (rotation/mirror), and no offset/clipping on
  the physical panel; the confirmed pins and parameters (or the corrections) are recorded in `## As built`
  and committed to the board profile only after this proof (ADR-0002 §5). Evidence is a photograph (the
  direct renderer has no canvas to dump, ADR-0084 decision 3).

- **Given** the `cyd_3248s035c` env with `SAPPER_DISPLAY_DIRECT_PANEL` and `SAPPER_BOARD_HAS_DISPLAY`
- **When** `pio run -e cyd_3248s035c` links
- **Then** it compiles and links clean: the panel config compiles, `DirectPanelDisplay` is selected over
  `LgfxDisplay`, there is **no** 307200 B `createSprite` for this board, `.dram0.bss` does not overflow, and
  the ADR-0082 internal-heap manifest path is unchanged. The RAM/Flash figures are recorded in `## As built`.

- **Given** a flashed CYD display build with confirmed pins
- **When** it boots
- **Then** the boot splash (name + `kFirmwareVersion` + tagline, `drawSplashFrame`, ADR-0041) renders on the
  ST7796 via the direct renderer, with no `[FATAL] display init failed` (a working panel is the designed
  state); `disp` reports the real geometry, not `0x0`. Evidence is a photograph.

- **Given** a flashed CYD display build running the endless hunt
- **When** it hunts
- **Then** no full-frame RGB565 buffer is allocated at runtime (`DirectPanelDisplay` has no canvas,
  `present()` is a no-op) and the panel shows the boot splash without flicker. The **live hunt HUD is
  deliberately deferred**: the sprite-era `Esp32ScreenRenderer` full-clears each heartbeat and is laid out
  for 240x135, so it would flash a 320x480 direct panel ~1 Hz; `hunt_loop` gates it off on a direct-panel
  board and logs `[SCREEN] splash + maintenance only`. Its 480x320-landscape re-layout + flicker-free
  region-repaint is a follow-on slice (LEDGER). Evidence is a photograph plus the `[SCREEN]` serial line.

- **Given** a flashed CYD display build
- **When** the operator BOOT-holds into Maintenance (`InputKind::Buttons`, unchanged from slice-0083)
- **Then** the Maintenance connection panel renders on the ST7796 (connection info + count only, never a
  PSK, ADR-0039 decision 11), confirming the Maintenance surface draws through the direct renderer.

## Design

**New decision, measured first.** The only genuinely new architecture is ADR-0084 (direct-to-panel
rendering on a board that cannot afford a full-frame sprite). Its number was measured before the decision:
a 320x480 RGB565 canvas is 307200 B against the CYD's measured 110580 B largest free block (ADR-0082), so
`createSprite(320, 480)` cannot succeed and even a half-frame 320x160 (102400 B) leaves ~8 KB headroom
while competing with TLS buffers. See ADR-0084.

**Reused, not rebuilt.**

- **The `IDisplay` seam and every surface (ADR-0002, ADR-0025, ADR-0039, ADR-0041).** `DirectPanelDisplay`
  implements the same `IDisplay` the splash, HUD, and Maintenance panel already draw through, so no surface
  changes except the flicker-control detail below. `resolveDisplay` and `panelAfterInit` are pure and
  unchanged; the native display tests stand.
- **The fail-loud init path (ADR-0045).** `DirectPanelDisplay::begin()` returns the real panel init result;
  `panelAfterInit` already routes a failed panel to the `NullDisplay`. The direct renderer is only the
  "real" arm; the failure path is untouched.
- **The capability-flag pattern (ADR-0002, ADR-0082).** `-DSAPPER_DISPLAY_DIRECT_PANEL` is set in the
  `cyd_3248s035c` env, the same build-time capability layer as `SAPPER_MANIFEST_HEAP_INTERNAL`. The
  instantiation site branches on it; the three existing boards never set it and keep `LgfxDisplay`
  byte-for-byte.
- **The panel probe (ADR-0041, ADR-0002 §5).** The existing `SAPPER_TEST_PANEL` proof renders the
  RGB/border/offset pattern; on this board its evidence is a photograph (no canvas dump, ADR-0084).

**Candidate pins (to be probe-confirmed on hardware before commit, ADR-0002 §5).** From the documented
ESP32-3248S035C-C pinout, consistent across ESP3D, espboards.dev, an ESPHome bring-up thread, and a tested
port PR (one conflicting retailer guide, for a different board, discarded):

- **ST7796 (SPI):** CS=15, DC=2, SCK=14, MOSI=13, MISO=12, backlight=27, RST tied (candidate -1).
- **GT911 (I2C), recorded for the deferred touch slice:** SDA=33, SCL=32, RST=25, INT=21. Caveat: ESP3D
  flags a routing mistake affecting the GT911 INT line on the C variant, and GPIO21 is shared, so a touch
  bring-up should prefer polling over the INT line until confirmed.

**Orientation.** The 320x480 glass is presented in **landscape (480x320), rotation 1**, to match the three
other boards (all landscape: 240x135, 320x170) and keep the deferred HUD in the same landscape layout family
as the shared `Esp32ScreenRenderer`. The 1-vs-3 right-side-up direction is a hardware fact confirmed on the
probe re-run (ADR-0002 §5); the native 320x480 controller RAM and the 307200 B canvas figure are unchanged
by rotation.

**Considered and deliberately not reused / not added:**

- **The sprite-backed `LgfxDisplay` (the whole point of ADR-0084).** Infeasible: 307200 B canvas vs 110580 B
  largest block. Not reused; `DirectPanelDisplay` replaces it for this board only.
- **Fork B: a banded sprite + retained draw-list (the rejected renderer fork).** A small band sprite pushed
  row-band by row-band is flicker-free with bounded RAM, but needs a display-list engine (fill/text ops
  replayed per band with clip + y-offset) because the full pixel frame cannot be held. That is materially
  more code for a surface the appliance does not have (no animation, no image blit). Rejected for KISS; if a
  future animated surface needs it, it supersedes ADR-0084 for that board.
- **Fork C: a half-frame 320x160 sprite.** 102400 B barely fits under 110580 B with ~8 KB headroom and
  competes with TLS handshake buffers during a sync; still needs Fork B's draw-list. Fragile, not simpler.
  Rejected.
- **GT911 capacitive touch / `InputKind::Touch`.** Deferred: no consumer (Maintenance entry is the BOOT-hold,
  ADR-0078). Re-logged in the LEDGER with the candidate I2C pins above. Building a touch input path with
  nothing to drive is the speculative generality the quality bar forbids (§3).
- **The live hunt HUD on this panel.** Deferred. The shared `Esp32ScreenRenderer` is the sprite-era renderer:
  it `fillScreen`s and redraws every frame (laid out for 240x135), which on a 320x480 direct panel would
  render cornered and flash ~1 Hz on each heartbeat change. Rather than ship that, `hunt_loop` gates the HUD
  surface off on a direct-panel board (`SAPPER_DISPLAY_DIRECT_PANEL`), so the splash and Maintenance panel
  (drawn-once) light the panel while the HUD waits. Its conversion to region-repaint (static chrome once; a
  changed field repainted over its own rect, never a whole-panel clear, ADR-0084 decision 4) plus the
  480x320-landscape re-layout is a follow-on slice, best tuned on the physical panel (ADR-0002 §5). Re-logged
  in the LEDGER.
- **No RGB LED, no SD.** Unchanged from slice-0083; still deferred (LEDGER).

## Verification

- **`pio run -e cyd_3248s035c`** (display build) is the machine check for the renderer selection and the
  no-overflow/no-full-frame-alloc claims: it compiles the ST7796 config, selects `DirectPanelDisplay`, and
  links within `.dram0.bss`. Figures recorded in `## As built`.
- **Native display tests** (`test/test_board_profile` for `resolveDisplay`/`panelAfterInit`) are unchanged:
  the resolution logic is pure and `DirectPanelDisplay` is device-only, so the native suite stands by
  construction. Any new pure helper added here ships its own test.
- **The `SAPPER_TEST_PANEL` panel probe** on the flashed CYD confirms the ST7796 pins/colour-order/rotation/
  offset on hardware (ADR-0002 §5); evidence is a photograph (no canvas dump on the direct renderer). The
  operator's to run, like the Stick/T-embed on-device proofs. `panel_probe` prints photographic-proof
  guidance (not "send `dump`") when the display has no canvas, and `scripts/0005` (`verify:device`) treats
  the empty dump (`bytes=0`) as a pass with a photographic-proof note rather than a false fail, so §4 #22's
  boot-face proof holds for this board via its photographic half (#38).
- **On-device boot + hunt + Maintenance** (not unit-assertable): the flashed build renders the splash, the
  HUD (buffer-free, no whole-panel flicker-clear), and the Maintenance panel on the ST7796; photographs are
  the evidence. The operator's to run.

## As built

Shipped the direct-to-panel renderer (`DirectPanelDisplay`, ADR-0084) and lit the CYD ST7796 as a
landscape surface. The panel probe confirmed every ADR-0002 §5 parameter on the physical glass on
2026-10-27; the one candidate that was wrong (`invert`) was corrected from the probe, re-run, and
re-confirmed.

**Panel-parameter proof (photographic, no canvas dump: ADR-0084 decision 3 / §4 #22/#38).** A
`SAPPER_TEST_PANEL` build (`cyd_3248s035c_testhooks`) rendered `drawBringupFrame` on the real panel. The
first run showed a whole-panel colour inversion (black field reading white, the R/G/B blocks reading
cyan/magenta/yellow), which identified the candidate `invert = true` as wrong. With `invert = false` the
re-run was correct and operator-confirmed eyes-on: a **black** field, the three top-left blocks reading
**red, green, blue** in order (so `rgb_order = false`, no R/B swap and no byte-order error), a white border
reaching all four edges (no offset/clipping), and a white diagonal hitting both corners. The frame is
**landscape, right-side-up at rotation 1** (480x320): the blocks sit in the top-left and the diagonal runs
top-left to bottom-right, which is `drawBringupFrame` on a 480x320 panel at rotation 1, so rotation 3 was
not needed. Evidence is the operator's photographs (the direct renderer has no canvas to dump; `panel_probe`
printed the photographic-proof guidance and `canvasByteLength()` reported 0).

**Confirmed pins/parameters** (now committed to the board profile and `lgfx_cyd.h`, ADR-0002 §5): ST7796 on
SPI2/HSPI, CS=15, DC=2, SCK=14, MOSI=13, MISO=12, backlight=27 (PWM ch7), RST tied (-1, the panel reset and
drew). `freq_write = 40 MHz` (clean frame), `spi_3wire = false` (MISO wired), `readable = true`,
`memory/panel = 320x480` native controller RAM, offsets 0, `invert = false`, `rgb_order = false`. Rotation 1
(landscape 480x320) is set in the board profile, not the panel config.

**Build (DoD scenario 2).** `pio run -e cyd_3248s035c` links clean: the ST7796 config compiles,
`DirectPanelDisplay` is selected over `LgfxDisplay` (`SAPPER_DISPLAY_DIRECT_PANEL`), **no** 307200 B
`createSprite` is allocated for this board, `.dram0.bss` does not overflow, and the ADR-0082 internal-heap
manifest path is unchanged. **RAM 29.1% (95352 B of 327680), Flash 44.1% (1388227 B of 3145728)** (+~1.1 KB
RAM over the headless slice-0083 build, well within budget). The reference Cardputer build is byte-for-byte
unchanged (gc-sections drops the unused `DirectPanelDisplay`), and native 340/340 stands (the display code
is device-only; `resolveDisplay`/`panelAfterInit` are pure and untouched).

**Boot splash / init (DoD scenario 3).** The panel initialised successfully: the probe frame rendered on a
live panel with no `[FATAL] display init failed`, and `main.cpp` draws `drawSplashFrame` (name +
`kFirmwareVersion` + tagline) on that same init path before dispatching the probe, so the splash renders
through the direct renderer by the same `drawText` primitive the proof exercised. A dedicated normal-build
splash photo, the live-hunt boot, and the Maintenance panel (DoD scenarios 4 and 5) are the operator's to
capture on a provisioned device, like the Stick's and T-embed's deferred on-device proofs (slice
`## Verification`; LEDGER).

**Live hunt HUD (DoD scenario 4): deferred as designed.** `hunt_loop` gates the sprite-era
`Esp32ScreenRenderer` off on a direct-panel board (`SAPPER_DISPLAY_DIRECT_PANEL`) and logs `[SCREEN] splash
+ maintenance only`; no full-frame RGB565 buffer is allocated at runtime (`DirectPanelDisplay` has no
canvas, `present()` is a no-op). The 480x320-landscape re-layout plus flicker-free region-repaint is a
follow-on slice (ADR-0084 decision 4; LEDGER).
