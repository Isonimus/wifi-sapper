---
id: '0104'
title: "Resolution-aware boot faces — splash, Maintenance, provisioning across three panel tiers"
type: slice
status: accepted
date: 2026-10-30
supersedes: []
superseded_by: []
---

## Goal

Make the three fixed-coordinate boot faces resolution-aware so every one uses the full
panel instead of drawing a 240x135 layout top-left on a larger display. This closes the
LEDGER `[deferred]` item: slice-0088 made the live hunt HUD (`Esp32ScreenRenderer`)
resolution-aware (two photo-tuned tiers selected by panel height), but the boot faces stayed
fixed-coordinate, tuned for 240x135, so a T-embed (320x170) or a CYD (480x320) wasted the
rest of its panel at boot. The three screens:

- the product splash (`drawSplashFrame`, ADR-0041),
- the Maintenance status screen (`drawMaintenanceScreen`, ADR-0039),
- the first-boot provisioning instructions screen (`drawProvisioningScreen`, ADR-0089, which
  deliberately matched the fixed-coordinate sibling style for consistency rather than shipping
  the lone resolution-aware boot screen).

**Why three tiers, not slice-0088's two.** The HUD only ever served two *sprite* sizes
(240x135 and the T-embed's 320x170); the CYD's 480x320 is a separate renderer
(`DirectPanelScreenRenderer`, ADR-0086) with its own hand-tuned constants. The boot faces
have no such split: all three are a single `IDisplay` path that `main.cpp` drives on **every**
panel, the CYD's `DirectPanelDisplay` included (`drawSplashFrame(panel)` at `main.cpp`,
`drawMaintenanceScreen`/`drawProvisioningScreen` likewise). So one function must serve three
genuinely different sizes on the bench — 240x135 (Cardputer, Stick), 320x170 (T-embed),
480x320 (CYD) — and a two-tier split would lump the CYD into the T-embed's 320x170 tier,
leaving the biggest and most-visibly-broken panel half-used. Three tiers is slice-0088's
"explicit eyeballed tiers" philosophy extended to the third size that actually exists.

This is part of the still-open slice-8 umbrella (broaden hardware + per-profile UI tuning,
LEDGER); it does not close it.

## Definition of Done

- **Given** a CYD (480x320, direct panel)
- **When** any of the three boot faces renders
- **Then** it uses the **large** tier and lays its content across the 480x320 panel (larger
  fonts, panel-derived spacing), not the top-left 240x135 corner, and no title or line is
  clipped or drawn off-panel.

- **Given** a LilyGo T-embed (320x170, sprite-backed)
- **When** any of the three boot faces renders
- **Then** it uses the **roomy** tier across the full 320x170 panel, nothing clipped.

- **Given** a 240x135 board (Cardputer or M5StickC Plus2)
- **When** any of the three boot faces renders
- **Then** it uses the **compact** tier and every title and line still fits within 240x135
  (no regression off the panel the current layout was tuned for).

- **Given** any panel height `h`
- **When** `bootFaceLayoutFor(h)` selects a tier
- **Then** it returns **compact** for `h < 160`, **roomy** for `160 <= h < 280`, and
  **large** for `h >= 280` — the boundaries unit-tested, so the CYD can never silently fall
  into the roomy tier through an off-by-one in the threshold (the exact bug this slice fixes).

- **Given** the tallest boot face (provisioning, six content slots)
- **When** each tier's constants are compiled
- **Then** a `static_assert` proves its lowest line stays on that tier's design panel, so a
  coordinate nudge that overflows fails the build, not the panel at runtime (mirrors the two
  HUD renderers' `static_assert`).

- **Given** a provisioned display board entering Provisioning or Maintenance
- **When** the boot/log path runs
- **Then** `[PORTAL]`/`[MAINT]` logs with no `[FATAL]` (the existing scripts/0007 and
  scripts/0005 error gates), and all four display envs link with their sprite/direct
  footprints unchanged by this cosmetic slice.

## Design

**Existing code considered.** `Esp32ScreenRenderer` (`src/surface/screen_renderer_esp32.cpp`)
is the pattern being ported: its `struct Layout` + `constexpr kCompact/kRoomy` tiers +
`kRoomyMinHeight` threshold + `layoutFor(height)` selector + `static_assert`-against-design-
height. That renderer's *big* `Layout` (20+ fields: bar Ys, banner strip, indicator step) is
**not** reused — it describes one dense HUD with bars and a banner, whereas a boot face is a
title plus a short stack of text lines, so a boot-face tier needs only `{margin, titleSize,
bodySize, lineGap}`. `drawWordmark` (`surface/wordmark.*`, slice-0097) **is** reused by the
splash unchanged; its shadow offset already scales with the glyph size, so a larger title tier
gets a proportional shadow for free. The two HUD renderers stay separate and untouched
(ADR-0086 decision 1); this slice does not merge or alter them.

**The three faces share a tier, not a layout.** The splash is a centred branding face (hero
wordmark + two lines); Maintenance and provisioning are top-aligned dense faces (section title
+ up to six lines). Forcing one Y-map across all three would be false DRY. Instead a single
pure `BootFaceLayout` tier carries only the shared *primitives* — left margin, title glyph
scale, body glyph scale, body line advance — and each face derives its own Y origin from them:
the splash centres its title block from `display.height()`, the info faces top-align at
`margin`. Content lines are placed at `bodyTop + slot * lineGap`, and a face inserts a visual
section break by **skipping a slot index** (provisioning uses slots 0,1,2,4,5 to gap before
"2. Open in browser"), so grouping survives without per-screen constants.

**Three tiers selected by panel height, in a pure host-tested module.** New header
`src/surface/boot_face_layout.h` (header-only `inline constexpr`, like the ADR-0057 key-decode
unit — no `build_src_filter` entry needed):

- `struct BootFaceLayout { int32_t margin; uint8_t titleSize; uint8_t bodySize; int32_t lineGap; };`
- `kCompact` (240x135), `kRoomy` (320x170), `kLarge` (480x320) — candidate values photo-tuned
  on glass (ADR-0002 §5); the exact numbers are a nudge, the tier *selection* is the invariant.
- `bootFaceLayoutFor(int32_t height)` returns `kLarge` for `height >= kLargeMinHeight` (280),
  `kRoomy` for `>= kRoomyMinHeight` (160), else `kCompact`. 280 sits above the T-embed's 170
  and at/below the CYD's 320, so the CYD gets `large` and the T-embed `roomy`; the branch is on
  panel capability, not board identity (ADR-0002), so a future panel of any size gets a sane
  tier with no new code.
- A `static_assert` per tier proves the tallest face (provisioning, max slot index 5) fits that
  tier's design height (135 / 170 / 320), mirroring both HUD renderers' guard.

**Why a pure module with a unit test, when slice-0088 kept its `Layout` device-only and
photo-only.** slice-0088's selection was trivial (one threshold, two tiers) and lived inside a
renderer that is device-only by nature. This slice has **two** thresholds, and `main.cpp` (where
the faces live) is not on the native lane at all, so the selector cannot be asserted where it
sits. Getting a threshold wrong is silent — the CYD would simply render the roomy tier and
under-fill, the precise failure this slice exists to prevent — so the selector earns a
regression test at its boundaries (`~/.claude/CLAUDE.md` §3). The per-tier *coordinates* stay
eyeballed (no test, photographic proof), exactly as slice-0088's were.

**Glyph metrics.** The `static_assert` needs the 6x8 default-font glyph dimensions, already
duplicated as file-local `kGlyphW`/`kGlyphH` in both HUD renderers. This header is the **third**
site, which trips the rule of three; rather than scope-creep a shared `glyph_metrics.h` into a
cosmetic slice (and touch the two frozen renderers), the extraction is deferred to LEDGER and
this header keeps a local `kGlyphH` with a comment pointing at the deferral.

**No ADR, no §4 row.** This is a layout refinement, not a durable mechanism, data format, or
boundary (slice-0088 reached the same conclusion for the HUD). The boot faces' photographic-
proof doctrine already exists and is unchanged: §4 #22 (splash/provisioning face) as refined by
#38 (direct-panel boards prove photographically, no canvas to dump) and ADR-0039 (Maintenance
screen) already say the coordinates are eyes-on; this slice only makes those coordinates tier-
selected. The resolution-awareness rationale lives here and in a code comment at the tier-
selection site so a future reader does not "restore" a hardcoded 240/135.

## Verification

The faces are **device-rendered**: their correctness is the on-air photograph (§4 #22/#38,
ADR-0041/0039/0089 are all photographic). The machine-checkable half guards the logic and the
boot path; the eyes-on half is the operator's per-board photos.

- **Unit test (machine-checked, native lane):** `test/test_boot_face_layout` asserts
  `bootFaceLayoutFor` returns compact/roomy/large at and across both boundaries (134/135,
  159/160, 170, 279/280, 320), covering DoD scenario 4. This is a new pure unit — the
  fail-before is the missing symbol (the suite does not link without the header), and it passes
  once the selector exists.
- **Compile-time (machine-checked):** the three `static_assert`s (DoD scenario 5) fail the
  build if any tier's provisioning layout runs off its design panel.
- **Build check (machine-checked):** the `cardputer`, `m5stickc_plus2`, `t_embed`, and
  `cyd_3248s035c` envs compile and link; the sprite allocation and `.dram0.bss` footprints are
  unchanged by this cosmetic slice.
- **Error gate (machine-checked, CI-wired):** `scripts/0005-cardputer-bringup-verify.mjs`
  (splash boot/dump half) and `scripts/0007-provisioning-verify.mjs` (`[PORTAL]` half) already
  assert no `[FATAL]`; unchanged, already wired. Covers DoD scenario 6's log half.
- **Native suite (machine-checked):** `ScreenView` and both HUD renderers are untouched, so
  `test/test_screen_*` pass unchanged — proof this slice added no HUD plumbing.
- **Photographic (eyes-on, operator-run):** splash, Maintenance, and provisioning photographed
  on at least one board per tier — a 240x135 board (compact, DoD scenario 3), the T-embed
  (roomy, scenario 2), and the CYD (large, scenario 1) — showing each face filling its panel
  with nothing clipped. Per the photographic-proof doctrine for buffer-constrained/device
  renderers. Any photo not taken in this work session is tracked as a deferred `[audit]` in
  LEDGER, as slice-0085/0088 tracked their deferred panel photos.

## As built

Shipped as designed. The three boot faces are resolution-aware across three tiers, selected by panel
height, and every tier was confirmed eyes-on on its board in one bench sweep (2026-10-30).

- New pure header `src/surface/boot_face_layout.h`: `BootFaceLayout` carries only the four shared
  primitives `{margin, titleSize, bodySize, lineGap}` (the HUD's 20-field Layout was deliberately not
  reused). `kCompact` (240x135), `kRoomy` (320x170), `kLarge` (480x320) are selected by
  `bootFaceLayoutFor(height)` at knees `kRoomyMinHeight` = 160 and `kLargeMinHeight` = 280. Three
  `static_assert`s prove the tallest face (provisioning, body slot 5) stays on each tier's design panel.
  Header-only `inline constexpr`, so no `build_src_filter` entry was needed and the selector is host-tested.
- The three faces in `main.cpp` now derive their geometry from the tier and `display.height()`: the splash
  centres its title block from the panel height; `drawMaintenanceScreen`/`drawProvisioningScreen` top-align
  the title at `margin` and place body lines at `bodyTop() + slot * lineGap`, skipping body slot 3 for the
  section gap. A comment at each site and in the header warns against restoring the hardcoded 240/135.
- The body glyph size is bounded per tier by the longest line ("power-cycle to resume hunting", 29 chars),
  so compact and roomy stay at size 1 (320 px cannot fit that line at size 2) and only the 480 px large
  tier grows body text to size 2; the larger title (size 3/4/5) is each tier's visible gain.

**Machine checks (this session):** native suite 381/381 (the new `test/test_boot_face_layout` adds 5,
covering the selector at and across both boundaries plus tier distinctness and the slot-5 fit); the header's
three `static_assert`s compiled clean on the native lane; lint 0/0; `adr/INDEX.md` regenerated.

**Device proof (eyes-on, operator-confirmed 2026-10-30).** All four display envs were flashed and linked
with no `.dram0.bss` overflow and no `[FATAL]` at boot (the scripts/0005 and scripts/0007 error-gate half,
DoD scenario 6): `cyd_3248s035c`, `m5stickc_plus2` (+ `m5stickc_plus2_testhooks`), `t_embed`, `cardputer`.
The three faces (splash, Maintenance, provisioning) were eyeballed on each tier and confirmed to fill the
panel with nothing clipped:

- **Large (CYD 480x320), scenario 1:** all three faces fill the panel (direct-panel board, so the proof is
  photographic/eyes-on per §4 #38, no canvas to dump).
- **Roomy (T-embed 320x170), scenario 2:** all three faces span the full panel, the case this slice most
  needed, since the old fixed layout drew into the top-left 240x135 corner and left the rest black.
- **Compact (Stick and Cardputer 240x135), scenario 3:** all three faces fit with no regression off the
  panel the pre-slice layout was tuned for.

No photographic artifact was committed: the boot faces are eyes-on/photographic proof by doctrine (§4
#22/#38, ADR-0041/0039/0089), and the operator's on-glass confirmation is that proof, recorded here exactly
as slice-0088 recorded its on-glass scenarios.

**Same-sweep Workstream B (not part of this slice, recorded for the session):** the Maintenance dashboard
browser serve was confirmed eyes-on on the Stick and the Cardputer (the loopTask-stack fix of commit
3548e76, previously proven only on the CYD, now proven on all three on-hand classic/S3 boards; its LEDGER
item is closed). The Stick's gate-C upload/stream-classify/drain path was confirmed alive and regression-free
on air; gate C's `markSubmitted` suppression itself stays open pending a real terminal Accepted/Duplicate
(a bonus data point already proven on the CYD; LEDGER line updated, not closed).

No ADR and no §4 row: this is a layout refinement, not a durable mechanism/boundary (slice-0088's call for
the HUD). The glyph-metrics `kGlyphH` duplication reached its third site here and is deferred to a shared
`glyph_metrics.h` extraction (LEDGER, rule of three).
