---
id: '0117'
title: "HUD collision fix: capture target and B/M1-M4 indicators on their own rows"
type: slice
status: accepted
date: 2026-11-12
supersedes: []
superseded_by: []
---

## Goal

Stop the compact-tier HUD from mangling the capturing line, and land the two
smallest, hardware-cheap clauses of the ADR-0116 redesign ahead of the full
hierarchy work. On the 240x135 compact tier (Cardputer, M5StickC Plus2) the capture
target label and the Beacon/M1-M4 indicator row share one line (`huntIndY == huntY`),
and the label is clamped only to its buffer, not to the pixels before the indicators,
so a 17-char BSSID (102px from margin 4) paints under the indicators that start at
x=88 (`:FF` overruns `B/1/2/3/4`). This is ADR-0116 decision 3 (identity and progress
never share a row) and decision 4 (the B/M1-M4 labels) applied as a minimal fix; the
four-band hierarchy, RSSI, armed-TX indicator and the PMKID badge remain the deferred
redesign slice (LEDGER, ADR-0116). No ledger item is closed here: the ADR-0116
redesign line stays open because this is a subset of it.

## Definition of Done

- **Given** the compact tier is capturing a hidden-SSID target (so the 17-char BSSID
  is the label)
- **When** the HUD renders the capturing line
- **Then** the BSSID occupies its own full-width row and the `B/M1/M2/M3/M4`
  indicator row sits on a separate row below it, with no glyph of the address painting
  under any indicator (the mangling is gone by construction, not by tuning).

- **Given** any tier is capturing
- **When** the indicator row renders
- **Then** the five indicators read `B M1 M2 M3 M4` (the EAPOL message vocabulary),
  not the bare `B 1 2 3 4`, on every renderer (compact, roomy, direct-panel).

- **Given** a capturing target whose name is wider than the panel row
- **When** the label renders
- **Then** it is clamped to the characters that fit the row width at the tier's font
  size (a belt-and-braces guard), never overflowing the panel and never reaching the
  indicator row (which is now a separate row anyway).

## Design

Searched the two sprite-renderer tiers (`screen_renderer_esp32.cpp`) and the
direct-panel renderer (`direct_panel_screen_renderer.cpp`). The roomy tier
(`huntIndY=128 != huntY=110`) and the direct-panel renderer (`kHuntIndY=214 !=
kHuntY=190`) already give the indicator row its own line and are immune; only the
compact tier crams them together. So the fix is confined to the compact `Layout` plus
the two shared pieces (the label clamp and the indicator labels).

Compact `Layout` changes (photo-tuned candidates, ADR-0002 §5): `huntIndY` 76 -> 86
(its own row below the label at 76), `huntBarY` 84 -> 96 (below the indicators),
`indStep` `2*kGlyphW` -> `3*kGlyphW` (18px, room for a 2-char `M1`..`M4` at size 1).
The indicator row and bar at y>=86 live in the same vertical region the CAPTURED/
CRACKED banner overpaints when active (`bannerY=86`), so no new vertical budget is
needed and the banner still takes over that strip when it fires; when it is not
firing (the common case) the rows are visible with y98..135 free below.

Once the compact tier gets its own indicator line, **every** tier is `ownLine`, so the
former shared-line branch in `drawHuntLine` (`ownLine ? ... : margin + 14*kGlyphW`)
becomes dead and is removed (no dead code, global §3): indicators always start at the
margin and advance by `L.indStep`.

The label clamp becomes width-correct instead of buffer-bounded: the characters that
fit the now-dedicated row are `(w - 2*margin) / (kGlyphW * lineSize)`, and the
`%.*s` precision is set to that, so a 32-char SSID shows in full on the 240px compact
row (38 chars fit) while a very long name is clamped to the 25 chars the 320px roomy
row holds at size 2, never overflowing either panel. `snprintf`'s own `sizeof` bound
keeps the buffer safe regardless.

The `B/1/2/3/4` -> `B/M1/M2/M3/M4` relabel is a one-line change to the `row[]` literal
in **both** renderers; the direct-panel `kIndStep` (48px) and the roomy `indStep`
(48px) already hold a 2-char label, so no other tier constant moves. No `ScreenView`
field changes (the `hasM1..hasM4` booleans and the surface policy are untouched), so
the host suite is unaffected by construction; the change is device draw code.

## Verification

No unit test can assert panel layout (the sprite renderers have a readable canvas but
the fake renderer records the `ScreenView`, not pixels; the direct-panel has no
canvas). Per §4 #22/#38/#39 the HUD is photographically verified:

- `scripts/0034-hunt-hud-verify.mjs` (`verify:hunt-hud`, already wired in
  `package.json`): its machine half asserts `[SCREEN] enabled` and no `[FATAL]` on a
  hooks build driving a lit HUD; unchanged and still green.
- The layout itself (the BSSID on its own row, the `B M1 M2 M3 M4` labels, no
  overlap) is **photographic**, eyes-on the physical compact panels (Cardputer,
  M5StickC Plus2) and the direct-panel CYD for the relabel, the operator's to run on
  attached hardware (LEDGER, like every HUD-layout proof). The native suite
  (`test/test_screen_toast_surface` et al.) must stay green, proving the surface
  policy and `ScreenView` are untouched.

## As built

Shipped exactly as designed. `screen_renderer_esp32.cpp`: the compact `Layout` moved
`huntIndY` 76->86, `huntBarY` 84->96, `indStep` to `3*kGlyphW`; `drawHuntLine` lost
the dead shared-line branch (all tiers are `ownLine`), the label buffer widened to 40
with a width-correct `%.*s` clamp from `(w - 2*margin)/(kGlyphW*lineSize)`, and the
`row[]` labels became `B/M1/M2/M3/M4`. `direct_panel_screen_renderer.cpp`: the same
`row[]` relabel (its own-line layout and `kIndStep` already fit). No `ScreenView` or
surface-policy change. Native suite green; `npm run lint` and `npm run index` green.
The eyes-on compact-panel and CYD photographs are the operator's to run on hardware
(tracked with the ADR-0116 redesign proof in the LEDGER).
