---
id: '0087'
title: "CYD live hunt HUD: a direct-panel ScreenRenderer with region-repaint"
type: slice
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Goal

Light the **live hunt HUD** on the CYD's 480x320 ST7796 panel, the one display surface slice-0085 left
deferred. slice-0085 shipped the boot splash and the Maintenance panel (both drawn-once) through the
buffer-free `DirectPanelDisplay` (ADR-0084) but gated the HUD off, because the shared sprite-era
`Esp32ScreenRenderer` full-clears the panel every ~1 Hz heartbeat (flashing a buffer-free panel) and is laid
out for 240x135 (rendering cornered on the big panel). This slice builds the follow-on ADR-0084 decision 4
named: a second renderer, `DirectPanelScreenRenderer`, that lays the HUD out for 480x320 landscape and
repaints only changed fields (static chrome drawn once), so the live HUD shows flicker-free on the direct
panel while the three sprite boards keep their proven renderer untouched.

This closes the `[feature] CYD live hunt HUD` ledger item. Its structure (a second renderer rather than
converting the shared one) is ADR-0086, which also means the ledger item's anticipated cost, re-verifying the
HUD on the three sprite boards, is **avoided**, not paid: `Esp32ScreenRenderer` is not touched.

## Definition of Done

- **Given** the `cyd_3248s035c` display env (`SAPPER_DISPLAY_DIRECT_PANEL`)
- **When** `pio run -e cyd_3248s035c` links
- **Then** `ActiveScreenRenderer` resolves to `DirectPanelScreenRenderer`, it compiles and links, `.dram0.bss`
  does not overflow, and no full-frame RGB565 sprite is allocated for this board (the renderer draws through
  the canvas-less `DirectPanelDisplay`, ADR-0084). RAM/Flash figures recorded in `## As built`.

- **Given** the three sprite boards (Cardputer, M5StickC Plus2, T-embed)
- **When** each is built
- **Then** `ActiveScreenRenderer` resolves to `Esp32ScreenRenderer` unchanged and the reference Cardputer
  build is byte-for-byte identical to before this slice (the HUD renderer did not change for these boards, so
  no on-air re-verification is required, ADR-0086 decision 1). Confirmed in `## As built`.

- **Given** a flashed CYD display build running the endless hunt
- **When** it hunts and the heartbeat toggles each second with no other field changing
- **Then** the HUD updates **without a whole-panel flash**: the title/chrome is drawn once and only the
  heartbeat indicator repaints (region-repaint, ADR-0086 decision 3 / ADR-0084 decision 4). `hunt_loop` logs
  `[SCREEN] enabled (...)`, not `[SCREEN] splash + maintenance only`. Evidence is a photograph/video plus the
  `[SCREEN]` serial line (the direct renderer has no canvas to dump, ADR-0084 decision 3).

- **Given** a flashed CYD display build hunting
- **When** the engine is Discovering, then Capturing a target, then a handshake is captured
- **Then** the HUD renders the live-hunt section for 480x320 (the SCAN line, then the target + the B/M1-M4
  indicator row + the progress bar), and a CAPTURED/CRACKED banner shows in its own lower strip without
  disturbing the HUD fields above it (ADR-0086 decision 3). Evidence is a photograph.

## Design

**No new measurement needed; the numbers are slice-0085's.** The 307200 B canvas vs 110580 B largest-block
figure (ADR-0082/ADR-0084) is why there is no sprite to clear atomically, which is why region-repaint is the
flicker fix. The flicker cadence (~1 Hz) is `kHeartbeatHalfPeriodMs` read from `ScreenToastSurface`. See
ADR-0086 for the decision and ADR-0084 decision 4 for the deferral this picks up.

**Reused, not rebuilt.**

- **The `ScreenRenderer`/`ScreenView` seam and the pure `ScreenToastSurface` policy (ADR-0025, ADR-0033).**
  `DirectPanelScreenRenderer` implements the same `ScreenRenderer` the surface already drives and consumes the
  same `ScreenView`, so the surface, the view-model, and all native tests are unchanged. The HUD content
  (which fact maps to which line, the banner arming) stays the surface's job.
- **The capability-flag + typedef-selection pattern (ADR-0002, ADR-0084).** `ActiveScreenRenderer`
  (`surface/active_screen_renderer.h`) selects the renderer on `SAPPER_DISPLAY_DIRECT_PANEL`, exactly as
  `ActiveLgfxPanel` (`lgfx_panel_select.h`) selects the panel class and `display()` selects the `IDisplay`
  backend. One place, one flag.
- **The `IDisplay` primitives (`fillScreen`/`drawPixel`/`drawText`/`present`).** The direct renderer uses the
  same minimal seam; `present()` is the `DirectPanelDisplay` no-op. No seam growth.

**Considered and deliberately not reused / not added:**

- **Converting `Esp32ScreenRenderer` to region-repaint for all boards (the fork rejected in ADR-0086
  decision 1).** DRY, but it changes three proven boards for zero user-visible benefit there and forces
  on-air photographic re-verification of each. Rejected to keep the sprite boards byte-for-byte, the repo's
  grain (ADR-0080/0082/0084).
- **A shared pure field->text/colour helper used by both renderers.** This is the **second** instance of the
  `ScreenView`->glyph mapping; the quality bar says duplicate at two, abstract at three (§3, rule of three).
  The small status-word/palette/line-format semantics are duplicated into the new renderer; the shared
  extraction is deferred to a third device renderer (LEDGER). Extracting now would also touch
  `Esp32ScreenRenderer`, defeating decision 1.
- **Double-buffering / a banded sprite (slice-0085 Fork B).** Rejected there for KISS on a text HUD; still
  rejected. Region-repaint is the buffer-free flicker control for a refreshing text surface (ADR-0084
  decision 4).
- **Sharing the banner's HUD-overlap behaviour from the 240x135 renderer.** On 480x320 there is room to give
  the banner its own strip below the HUD, so arming/clearing it repaints only that strip and no HUD field sits
  under it. Simpler than the small panel's overpaint-and-redraw, afforded by the larger geometry.

**Layout.** 480x320 landscape (rotation 1, slice-0085). Named constants for every field position and font
size; a static title drawn once; the status word + a heartbeat square, the up/cracks counters, the sync line,
the live-hunt line + progress bar, and a banner strip at the bottom. Per ADR-0086 decision 5 the exact
coordinates are candidate values to be photo-tuned on the physical panel (ADR-0002 §5); the region-repaint
logic is what this slice must get right.

## Verification

- **`pio run -e cyd_3248s035c`** (display build) is the machine check for renderer selection, the no-overflow
  and no-full-frame-alloc claims (DoD scenario 1). Figures in `## As built`.
- **Sprite-board builds + native suite** are the machine check for "sprite boards unchanged" (DoD scenario 2):
  `Esp32ScreenRenderer` is not edited, so the Cardputer binary and native 340/340 stand by construction.
- **`scripts/0034-hunt-hud-verify.mjs`** (`verify:hud`): its error-gate half (no `[FATAL]`/`[ERROR]`,
  `[SCREEN] enabled`) runs unattended and is wired in `package.json` (R11). On the CYD the artifact half is a
  **photograph**, not a dumped PNG, because the direct renderer has no canvas (`canvasByteLength()==0`,
  ADR-0084 decision 3 / §4 #22/#38) — the same photographic-proof treatment slice-0085 used for the splash.
- **On-device hunt** (not unit-assertable, DoD scenarios 3-4): the flashed CYD renders the HUD flicker-free
  (heartbeat repaints only its indicator, no whole-panel flash), the live-hunt section for 480x320, and the
  banner in its own strip. Photographs/video are the evidence; the operator's to run, like slice-0085's
  deferred on-device proofs.

## As built

Shipped `DirectPanelScreenRenderer` (480x320 layout + region-repaint) and the `ActiveScreenRenderer`
selection header (ADR-0086); `hunt_loop` now wires the HUD on every display board and slice-0085's
direct-panel HUD gate is retired. `Esp32ScreenRenderer`, `ScreenToastSurface`, and `ScreenView` are unedited.

**Build (DoD scenario 1).** `pio run -e cyd_3248s035c` links clean with the renderer selected: **RAM 29.2%
(95752 B of 327680), Flash 44.2% (1391767 B of 3145728)** (+400 B RAM / +3540 B Flash over slice-0085).
`.dram0.bss` does not overflow and no full-frame RGB565 sprite is allocated (the renderer draws through the
canvas-less `DirectPanelDisplay`). Two compile-time `static_assert`s enforce the 480x320 banner layout budget
(the lowest line stays on-panel; the banner strip sits below the hunt band).

**Sprite boards byte-for-byte (DoD scenario 2), measured.** `Esp32ScreenRenderer` was not edited, and the new
renderer's translation unit is guarded by `SAPPER_DISPLAY_DIRECT_PANEL` so it compiles to **nothing** on a
sprite board (mirroring `direct_panel_display.cpp`). Proven by rebuilding the Cardputer firmware on the
pre-change tree and on this change: `.text`/`.data`/`.bss` are **identical** in every section (text 1030847,
data 374836, bss 2549873). An earlier version without the TU guard leaked +208 B of dead string literals into
`.data`; the guard removed it, restoring an exact section match. (The `.bin` md5 differs between builds only by
the build timestamp ESP-IDF embeds in the app descriptor, which is why section sizes, not md5, are the
byte-for-byte-code measure.) Native suite 340/340, unchanged by construction (the pure policy did not move).
So the three sprite boards need no on-air re-verification, the cost ADR-0084 decision 4 / the closed ledger
item anticipated and ADR-0086 decision 1 avoids.

**Adversarial correctness pass (Sonnet, blind to intent).** Found and fixed one High-severity defect: the
banner BSSID line was drawn at y=310 at 16 px height, reaching row 326 on a 320-row panel (6 rows off-panel),
which an ST7796 can wrap into visible top-row corruption, on every CAPTURED/CRACKED toast (a common event).
Fixed by re-spacing the three banner lines to fit the 64 px strip (title 260, essid 284, bssid 302 -> 317) and
adding the `static_assert` layout-budget guard above, which fails on the pre-fix 326 > 320 and passes after
(the fails-before/passes-after regression check, in the only form a compile-time layout constant admits; this
device renderer is otherwise photographically verified, not unit-tested, per the repo seam). The reviewer
cleared field-to-region coverage (no `ScreenView` field dropped from change detection), the first-render path,
the heartbeat/status-colour interaction, the hunt-band repaint, and the X-axis bounds.

**On-device HUD (DoD scenarios 3-4): photographic, the operator's to run.** The flicker-free HUD (heartbeat
repaints only its indicator, no whole-panel flash), the 480x320 live-hunt section, and the banner in its own
strip are confirmed on the physical panel by photograph/video (the direct renderer has no canvas to dump,
ADR-0084 decision 3), like slice-0085's deferred splash/Maintenance photos. The 480x320 field coordinates are
candidate values to photo-tune within the asserted budget (ADR-0086 decision 5). Logged as a LEDGER audit
item.
