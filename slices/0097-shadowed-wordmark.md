---
id: '0097'
title: "Shadowed product wordmark: draw WiFi Sapper with the banner's magenta drop shadow"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Bring the on-panel UI closer to the project banner (`docs/images/wifi-sapper-banner.webp`) for a unified
look. The banner renders the `WiFi Sapper` wordmark as **white glyphs inside a magenta neon glow**, and the
tagline (`autonomous handshake hunter`) in **cyan**. The display seam cannot blur, so a soft glow is not
drawable; the wordmark is instead drawn with white glyphs over a **magenta drop shadow**, the achievable
emulation that keeps the glyphs legible.

The wordmark is drawn as glyphs in three places, today disagreeing on colour: the sprite HUD title
(`screen_renderer_esp32.cpp`, white), the direct-panel HUD title (`direct_panel_screen_renderer.cpp`, white),
and the boot splash (`main.cpp` `drawSplashFrame`, green). All three move to the shadowed wordmark together,
so the name reads the same across every screen. The splash tagline also moves to cyan to match the banner.
The Maintenance dashboard's HTML `<h2>` is the fourth name site (LEDGER product-name item) and is **out of
scope**: it is served HTML, restyled via CSS in the portal slice next.

Only the wordmark/tagline colours change. The semantic HUD colours (green hunting / blue working / amber
degraded / red fault, and the high-contrast white CAPTURED/CRACKED banner strip) are untouched: this is a
brand treatment on static chrome, not a signal. The on-device Maintenance *panel*'s green info lines are also
out of scope (not a title).

## Definition of Done

- **Given** the product wordmark drawn at any site
- **When** `drawWordmark` renders it
- **Then** it issues exactly two draws in order: a **magenta** copy offset down-right, then the **white**
  glyphs centred and drawn last (on top of the shadow), both with the same text and size; the shadow offset
  scales with the glyph size and is never zero.

- **Given** a board rendering the live hunt HUD (sprite tier or direct-panel tier)
- **When** the HUD title row is drawn
- **Then** the `WiFi Sapper` wordmark renders white over a magenta drop shadow, and every semantic colour
  (status word, heartbeat dot, SCAN line, counters, CAPTURED/CRACKED banner) renders exactly as before.

- **Given** any board at boot (including the screenless-to-splash path via `NullDisplay` no-ops)
- **When** `drawSplashFrame` draws the boot face
- **Then** the `WiFi Sapper` wordmark renders white over a magenta drop shadow (not flat green) and the
  tagline renders cyan (not white); the version line is unchanged.

## Design

**Existing code considered.** The two HUD palettes already carry `kCyan = 0x07FF` (used for the tagline on the
splash); pure magenta `0xF81F` is new, chosen over the banner's softer violet `~#C04BFF` because the brighter
pure hue reads as a distinct shadow and stays legible on black. The `IDisplay` seam exposes only
`drawText`/`drawPixel`/`fillScreen` with no alpha or blur (ADR-0025 decision 3), so a true soft glow is
unreachable without widening the seam, far too much machinery for static chrome; the hard drop shadow is the
achievable emulation.

**Shadow tuned on the panel (measure-don't-estimate).** A two-channel RGB-split (a cyan copy up-left and a
magenta copy down-right, emulating chromatic aberration) was tried first and read **too heavy** on the CYD's
480x320 size-4 title: the two fringes at `size/2 + 1` px (3 px) doubled the wordmark's apparent weight. It was
reduced on-panel to a **single magenta drop shadow at half the offset** (`size/2`, floored at 1: a size-4
title drops 2 px, size-3 drops 1 px). The white-over-magenta layering is the invariant; the offset is a
photo-tuned candidate (ADR-0002 §5).

**Rule-of-three extraction.** The effect is identical 2-layer logic at all three wordmark sites, so it is the
third instance that triggers extraction (§3): a single `drawWordmark(IDisplay&, x, y, text, size)` free
function (`surface/wordmark.*`) owns the colours, the layer order (white last), and the size-scaled offset, so
the effect is tuned in one place. This is a **leaf drawing utility**, not a merge of the two renderers: §4
#39's warning is against DRYing the two `ScreenRenderer` *classes* (their region-repaint logic stays
duplicated); both still call it exactly as they call `display_.drawText`. It does **not** own the product-name
string (passed as `text`), so the separate product-name-constant deferral (LEDGER) is unchanged: the four name
sites still spell the string themselves (one is an HTML literal, dissimilar).

**Call-site change only for the palette.** `kWhite` is shared by the title and the CAPTURED/CRACKED banner, so
no palette constant is renamed or removed; the renderers just call `drawWordmark` instead of one `drawText`.
`main.cpp` keeps `kGreen` (still used by the Maintenance panel lines) and gains a local `kCyan` for the
tagline.

## Verification

`drawWordmark` is pure over the `IDisplay` seam, so its colour/offset/order logic is **host unit-tested**
against a recording fake (no LovyanGFX), unlike the renderers' on-panel layout which stays photographic:

- **Unit test (machine-checked), DoD scenario 1:** `test/test_wordmark` drives `drawWordmark` against a
  `FakeDisplay` that records every `drawText` call and asserts the two layers, the magenta-then-white order,
  the down-right offset, and that the offset scales with size and never reaches zero. Wired natively
  (`+<surface/wordmark.cpp>` in the `native` env). It fails before the helper exists and passes after.
- **Photographic (operator-run), DoD scenarios 2 and 3:** on each panel tier, confirm the HUD title and the
  boot splash wordmark render white over a legible magenta shadow, the tagline cyan, and every semantic colour
  unchanged. Tiers: compact 240x135 (Cardputer/Stick, sprite), roomy 320x170 (T-embed, sprite), direct-panel
  480x320 (CYD). No new `scripts/*-verify.mjs` (the runtime path is unchanged); the existing `scripts/0034`
  HUD and `scripts/0005`/`scripts/0007` boot error-gates still pass (no `[FATAL]`).
- **Native suite + four-board builds (machine-checked):** `npm test` and the per-board compiles stay green.

## As built

Shipped as designed: a single `drawWordmark` helper (`surface/wordmark.*`) draws the product wordmark as
white glyphs over a magenta drop shadow (down-right, offset `size/2` floored at 1), called in lockstep by the
sprite HUD renderer, the direct-panel HUD renderer, and the boot splash; the splash tagline moved to cyan. No
semantic HUD colour changed. The product-name string stayed inlined at each site (the LEDGER deferral is
untouched); the served-HTML dashboard title is left for the portal slice (LEDGER).

**Effect tuned on the panel (measure-don't-estimate).** The first on-panel candidate was a two-channel
RGB-split (cyan up-left + magenta down-right at `size/2 + 1` px); the operator found it too heavy on the CYD's
480x320 size-4 title, so it was reduced to the single magenta drop shadow at half the offset. The helper was
renamed from the interim `drawSplitTitle` to the effect-agnostic `drawWordmark` once the effect settled.

**Machine-checked.** Lint 0/0, `adr/INDEX.md` regenerated, native suite **373/373** including the new
`test/test_wordmark` (two layers, magenta-then-white order, down-right offset, offset scales with size and
never zero). All four envs build clean (`cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`); each
flashed board booted with `[SCREEN] enabled` and no `[FATAL]`.

**Photographic proof (operator-confirmed, 2026-10-28).** The HUD title was eyes-on confirmed on every distinct
renderer tier; the boot splash uses the identical `drawWordmark` call and rendered during each observed clean
boot. DoD scenarios 2 and 3 met:

- **CYD** (direct-panel `DirectPanelScreenRenderer`, 480x320, size-4, 2 px shadow): confirmed perfect
  (`disp=480x320`, `heap_max=110580`). This is the tier where the RGB-split read too heavy and the shadow was
  tuned down.
- **M5StickC Plus2** (sprite `Esp32ScreenRenderer`, compact 240x135, size-3, 1 px shadow): confirmed perfect,
  the subtlest tier still legible (`disp=240x135`, PSRAM `heap_max` 2.06 MB).
- **T-embed** (sprite `Esp32ScreenRenderer`, roomy 320x170, size-4, 2 px shadow): confirmed
  (`disp=320x170`, octal PSRAM `heap_max` 8.26 MB).
- **Cardputer** (sprite, compact, ESP32-S3): reference-covered, same compact-sprite path as the Stick and same
  S3 chip as the T-embed; built clean, not separately re-flashed (operator's call).

DoD scenario 1 is the `test_wordmark` unit test above.
