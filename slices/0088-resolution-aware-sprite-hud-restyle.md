---
id: '0088'
title: "Resolution-aware sprite HUD restyle + firmware version in the title"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Lift the sprite-board live hunt HUD (`Esp32ScreenRenderer`) toward the polish the CYD got
from its direct-panel renderer (slice-0087 / ADR-0086), and fix a latent layout bug the
same renderer carries. Two concrete things ship:

1. **Resolution-awareness.** `Esp32ScreenRenderer` hardcodes `240` and `135` throughout
   (`barW = 240 - 2*kMargin`, `kBannerY = 135 - ...`, `fillRect(0, kBannerY, 240, ...)`),
   but it serves **three** panels at two resolutions: Cardputer and M5StickC Plus2 at
   240x135, and the LilyGo T-embed at 320x170 (confirmed sprite-backed,
   `src/config/boards/t_embed.h`). So today the T-embed draws its entire HUD into the
   top-left 240x135 corner and leaves the rest of its panel black. After this slice the
   renderer derives its layout from `display_.width()`/`display_.height()` (the seam already
   exposes both; the direct renderer already reads them), so a bigger panel is used, not
   wasted.

2. **Firmware version in the HUD title.** The title becomes `WiFi Sapper v<version>`, read
   from the single `kFirmwareVersion` source (`src/config/firmware_version.h`) the splash
   already uses (`main.cpp` `drawSplashFrame`) — no magic version string, the spirit of
   §4 #22. Applied in **both** device renderers (sprite and direct) so every display board
   shows it; the two renderers keep their deliberately-duplicated title code (the
   keep-duplicating call of ADR-0086 decision 4, unchanged: still only two renderers,
   rule-of-three not yet tripped — LEDGER).

This is part of the still-open slice-8 umbrella (broaden hardware + per-profile palette/UI
tuning, LEDGER); it does not close slice-8, which stays open for the bare-ESP32 profile,
full keyboard bring-up, and the deferred T-embed/CYD peripherals.

Honest scope note: the visible payoff is concentrated on the **T-embed** (its HUD goes from
a corner to the full panel, with larger fonts it has the room for). The 240x135 boards keep
their already-tuned compact layout essentially unchanged; their wins are the version string
and extents that are now correct-by-derivation rather than correct-by-coincidence (their
`240`/`135` literals happened to equal their panel).

## Definition of Done

- **Given** a LilyGo T-embed (320x170, sprite-backed)
- **When** the live hunt HUD renders
- **Then** the HUD lays out across the full 320x170 panel (the roomy layout tier), not the
  top-left 240x135 corner, and no field or banner is clipped or drawn off-panel.

- **Given** any board with a display (Cardputer, Stick, T-embed, CYD)
- **When** the HUD renders
- **Then** the title reads `WiFi Sapper v<kFirmwareVersion>` (e.g. `WiFi Sapper v0.1.0`),
  sourced from the single `kFirmwareVersion` constant, fitting within the panel width.

- **Given** a 240x135 board (Cardputer or M5StickC Plus2)
- **When** the HUD renders
- **Then** it uses the compact layout tier (the pre-slice 240x135 layout, plus the version
  string) and every field still fits within 240x135.

- **Given** a provisioned display board entering the hunt
- **When** `huntLoopBegin` wires the HUD
- **Then** `[SCREEN] enabled` logs, no `[FATAL]` appears, and the sprite envs link with the
  full-frame sprite allocation and `.dram0.bss` footprint unchanged by this cosmetic slice.

## Design

**Existing code considered.** `Esp32ScreenRenderer` (`src/surface/screen_renderer_esp32.cpp`)
is the thing being restyled. `DirectPanelScreenRenderer` (slice-0087) already reads
`display_.width()`/`display_.height()` for its horizontal extents and is the pattern to
mirror; it is **not** reused or merged — ADR-0086 decision 1 keeps the two renderers
separate (sprite full-redraw vs direct region-repaint), and the user's steer this slice is
to keep the shared vocabulary duplicated, not extract it (the rule-of-three helper stays
deferred, LEDGER). `ScreenView` is **unchanged**: the version comes from `kFirmwareVersion`
inside the renderer (like the splash), not through a new view field, so the pure
`ScreenToastSurface` policy and its native fake-renderer tests are untouched by construction.

**Resolution-awareness: two photo-tuned layout tiers selected by panel size, not board
identity.** A small `Layout` aggregate is computed per render from `(width, height)`:
- Horizontal extents (bar width, banner width, field-clear rects) always derive from
  `width()`, so the T-embed's 320px-wide rows span its panel instead of stopping at 240.
- A **compact** tier (the current 240x135 constants, kept as-is so those boards do not
  regress) and a **roomy** tier (new, larger fonts and spacing for the taller/wider panel)
  are selected by a height threshold (`height >= kRoomyMinHeight`). This branches on what
  the panel *is big enough for*, in the spirit of ADR-0002 (capability, not board name), so
  a future sprite board of either size gets a sensible tier with no new code.

The tier *selection* is the invariant; the exact per-tier coordinates, font sizes, and the
threshold are **candidate values photo-tuned on the physical panels** (ADR-0002 §5), exactly
as the direct renderer's 480x320 constants were (ADR-0086 decision 5). A single proportional
formula was rejected: font size is a coarse integer multiplier, the result must be
eye-verified on glass regardless, and two explicit tiers of eyeballed numbers are more
reviewable than proportional math nobody confirmed pixel-by-pixel.

**No ADR.** This is a layout refinement and a latent-bug fix, not a durable mechanism, data
format, or boundary (the kinds §1/taxonomy reserve for an ADR). The resolution-awareness
rationale lives here in the slice Design and in a code comment at the tier-selection site so
a future reader does not "restore" the hardcoded `240`/`135`. §4 #22 is about the boot
splash/face, not the live HUD, so it is untouched; §4 #39's "sprite boards ship
byte-for-byte, no re-verification" was a historical claim scoped to slice-0087 (which did
not touch them) — this slice deliberately changes `Esp32ScreenRenderer` and re-verifies it
photographically, which is consistent with, not a violation of, that frozen claim.

## Verification

This is a **device renderer**: its correctness is the on-air photograph, not a unit test
(ADR-0025; the native lane exercises the pure surface via the fake renderer, which this
slice does not change). Proofs:

- **Error gate (machine-checked, CI-wired):** `scripts/0034-hunt-hud-verify.mjs` asserts
  `[SCREEN] enabled` and no `[FATAL]` when a provisioned board enters the hunt — unchanged
  and already wired in `package.json`. Covers the fourth DoD scenario's log/boot half.
- **Build check (machine-checked):** the `cardputer`, `m5stickc_plus2`, and `t_embed` envs
  compile and link; the full-frame sprite allocation and `.dram0.bss` footprint are
  unchanged by this cosmetic slice (a link-time regression would fail the build).
- **Native suite (machine-checked):** `ScreenView` is unchanged, so `test/test_screen_*`
  and the fake renderer pass untouched — proof the restyle added no plumbing.
- **Photographic (eyes-on, operator-run):** HUD photos of the **T-embed** (full-panel roomy
  layout, DoD scenario 1) and a **240x135 board** (compact tier + version, DoD scenario 3),
  plus the version string legible in both (DoD scenario 2). Per the photographic-proof
  doctrine for buffer-constrained/device renderers (§4 #22/#38, ADR-0086). If the photos are
  not taken in this work session they are tracked in LEDGER, like slice-0085's deferred
  splash/Maintenance photos.

## As built

Shipped as designed, with the photographic proof split by which board was connected.

- `Esp32ScreenRenderer` became resolution-aware: a `Layout` aggregate is selected per render by
  panel height (`kRoomyMinHeight` = 160 px) — a **compact** tier (240x135, the pre-slice layout
  kept intact so Cardputer/Stick do not regress) and a **roomy** tier (the T-embed's 320x170,
  larger title/status/body fonts and the vertical room the compact tier lacked). Horizontal
  extents (the progress bar, field-clear rects, the banner strip) derive from `display_.width()`,
  so the T-embed's rows span its panel instead of stopping at the old hardcoded 240. The tier is
  chosen by panel capability, not board identity (ADR-0002); a compile-time `static_assert` keeps
  the roomy banner's lowest line on the 170 px panel.
- Both device renderers show `WiFi Sapper v<kFirmwareVersion>` in the title, the version drawn
  smaller (`kDim`) after the product name, from the single `kFirmwareVersion` source the splash
  uses (no magic version string, §4 #22's spirit). `ScreenView` is unchanged, so the pure surface
  policy and its native fake-renderer tests are untouched. The two renderers keep their
  deliberately-duplicated title/palette/status code (ADR-0086 decision 4; still two renderers,
  rule-of-three not yet tripped — LEDGER).
- Machine checks this session: lint 0/0, index regenerated, native suite 340/340
  (`test_screen_toast_surface` green, confirming no view/plumbing change). All four display envs
  built SUCCESS with no `.dram0.bss` overflow on the classic boards (Stick, CYD); the CYD grew
  +108 B flash for the version string, RAM unchanged (95752 B).
- **DoD scenarios 2, 3, 4 met on device (2026-10-28):** the M5StickC Plus2 (240x135, classic
  ESP32) was flashed and reached the hunt — `disp=240x135` (so the **compact** tier was selected),
  `[SCREEN] enabled`, no `[FATAL]` — and the operator confirmed the `WiFi Sapper v0.1.0` title and
  the compact layout render fine on the panel.
- **DoD scenario 1 (T-embed roomy tier) is build-verified, not yet eyes-on:** the `t_embed` env
  selects the roomy tier and links, but no T-embed was connected this session, so the full-panel
  photo is tracked as a deferred `[audit]` in LEDGER (the slice-0085/0087 photographic-proof
  pattern the Verification section anticipated).
