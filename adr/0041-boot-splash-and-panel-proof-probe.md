---
id: '0041'
title: "Boot splash as the shipped face; the panel proof relocates behind a bench probe"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

The shipped boot face is `drawBringupFrame()` in `main.cpp`: three RGB565 primary blocks, a
one-pixel white border on all four edges, and a corner-to-corner diagonal. It is not a splash
— it is a **panel diagnostic**. The blocks make a colour-order/byte-order error obvious, the
border makes an offset or clipping error obvious, and the diagonal makes a mirror or rotation
error obvious; together they confirm the panel parameters (driver, offsets, rotation) that
ADR-0002 §5 deferred to slice-0005's bring-up and that slice-0005 confirmed empirically.

It carries that diagnostic duty because in slice-0005 it was the *only* thing to draw: there
was no status surface, no product identity, nothing else a boot could show. The code comment
says as much — *"Boot self-test / splash. It doubles as the ADR-0002 §5 panel proof."* That
double duty was an expedient, not a decision. The panel parameters are now long-confirmed and
re-confirmable on demand; the boot face no longer needs to be the thing that proves them.

What a user actually sees at power-on should identify the appliance. This is a field device
that runs headless-first and, when it has a screen, spends real seconds on the boot face
during the STA-connect window before a phase surface (portal / HUD / Maintenance panel)
replaces it. An engineering test pattern there tells an operator nothing — not which firmware
is flashed, not even that this is a WiFi Sapper. `drawText` is already device-proven on this
exact panel by slice-0026's screen HUD (`verify:screen`), so a real splash costs nothing new
to render correctly.

But the diagnostic must not simply be deleted. It is the one artifact that catches a
colour-order / offset / mirror / rotation regression when a new board profile is added or a
LovyanGFX bump changes a panel's behaviour — a class of failure no host test can see. Removing
the shipped face's diagnostic and having nowhere to run it would trade a real regression guard
for cosmetics.

## Decision

1. **The shipped boot face is a product splash, not a diagnostic.** `drawSplashFrame(IDisplay&)`
   replaces `drawBringupFrame()` in `setup()`: the product name (`"WiFi Sapper"`, large), the
   firmware version, and a one-line tagline, drawn with the `drawText` primitive slice-0026
   already proved on this panel. A screenless board's `NullDisplay` makes it a no-op, exactly as
   the bring-up frame was.

2. **The panel proof is preserved, relocated behind a `SAPPER_TEST_PANEL` bench probe.** The
   RGB-blocks + border + diagonal frame (and its `fillRect` helper and diagnostic colour
   constants) move out of `main.cpp` into `panel_probe.{h,cpp}`, gated on `SAPPER_TEST_PANEL`
   and dispatched among the other bench probes in `setup()`. Like every probe it is compiled out
   of every shipped build (aligns with §4 #4: the actuation/diagnostic vocabulary is absent
   without `SAPPER_TEST_HOOKS`), so no shipped binary can render the test pattern at boot — the
   diagnostic exists only when a hooks build is flashed to run it.

3. **`verify:device` serves both builds; no new verify script is added.** `scripts/0005-cardputer-bringup-verify.mjs`
   already dumps whatever canvas is on the panel and reconstructs a PNG. It now proves *both*
   faces from the same wired script: flashed as a **normal** build it dumps the *splash* for
   review; flashed as a `SAPPER_TEST_PANEL` hooks build it dumps the *panel proof* — the exact
   RGB/border/diagonal frame slice-0005 asserted, unchanged. A dedicated splash-verify would be
   the "runs once, dead thereafter" script §3 warns against: the splash is static content drawn
   with an already-device-proven primitive and has no data-dependent or timing behaviour, so
   there is nothing a per-slice regression script would guard that `verify:screen` (the primitive)
   and this dump (the composition) do not. Reusing the one script also avoids copying the
   serial+PNG dump machinery a third time (it already exists in slice-0005 and slice-0026; a
   third copy would trip the rule of three).

4. **A single firmware-version constant, `kFirmwareVersion`.** The version the splash shows is
   named once (`"0.1.0"`, the pre-release appliance's first tagged face) rather than a bare string
   in the boot path — one source of truth a later surface (a serial `state` line, the Maintenance
   dashboard) can read, and no magic value in the render.

This **extends** ADR-0002 (the board-profile/LovyanGFX display foundation) and does not supersede
it: the §5 panel parameters stand exactly as slice-0005 confirmed them; this decision only moves
*where* that confirmation runs and *what* the shipped face shows. It does not supersede slice-0005,
whose frozen account of the bring-up frame rendering on that day stays true.

## Consequences

- A new standing invariant (§4 #22): the shipped boot face renders only the product splash; the
  RGB/border/diagonal panel-parameter proof renders only under `SAPPER_TEST_PANEL`, absent from
  every shipped build. Enforced by `verify:device` (a dump of a `SAPPER_TEST_PANEL` build still
  reconstructs the diagnostic frame) plus review (no diagnostic-drawing primitives on the shipped
  boot path).
- `scripts/0005-cardputer-bringup-verify.mjs` gains a header note documenting the two-build usage;
  its assertions do not change (it still dumps a canvas and reconstructs a PNG). `README.md`'s
  `verify:device` entry is updated to say which build proves which face.
- A future board profile that renders differently is caught the same way it always was — flash the
  `SAPPER_TEST_PANEL` build and dump — so the regression guard slice-0005 built is not lost, only
  moved off the shipped face.
- The splash uses only `fillScreen` + `drawText`, primitives already on the `IDisplay` seam; no new
  seam method, no host-test change (the splash is a renderer concern with no view-model logic, so
  it follows the screen-renderer precedent: device-only, PNG-verified, not host-tested).
- Controls to enter the panel probe from a shipped build are deliberately absent: a shipped unit's
  operator never needs the diagnostic, and a runtime path to it would be exactly the shipped-face
  test pattern this ADR removes.
