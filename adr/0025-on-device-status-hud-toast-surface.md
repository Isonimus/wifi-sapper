---
id: '0025'
title: "On-device status-HUD + cracked-toast display surface"
type: architecture
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Context

The Cardputer ADV has a 240x135 panel, but the shipped firmware draws the ADR-0002 §5 bring-up
self-test once at boot and then leaves it static for the device's whole run (`drawBringupFrame`,
`main.cpp`). A static frame is indistinguishable from a hung board, and the panel — the one surface
an operator glances at when the box is in hand — carries no running information.

slice-7 makes each surface an event-bus observer (ADR-0021, ADR-0001). The status LED (slice-0022) is
the first, but it is *coarse*: one colour says "hunting / working / degraded / fault", and a single
flash says "something cracked" without saying **what**. The screen can say what the LED cannot — which
ESSID cracked, how many captures have uploaded, whether the last hourly sync succeeded — so it is the
natural home for the detail slice-0022 deliberately kept off the LED (slice-0022 Scenario J finding:
per-capture upload outcomes belong "in the serial log and, later, the … dashboard, not on this
one-colour indicator").

Two forces shape the mechanism:

1. **The display is a passive surface, not a transmitting one.** Rendering to a local panel needs no
   network, so this surface has none of ADR-0023's window-scoped machinery: it observes facts on the
   bus and renders on its own `tick()`, exactly like the LED. §4 invariant #14 (transmit only in an STA
   window) does not apply; invariant #13 (surfaces receive facts only via the bus, never reaching into
   engine state) already governs it, so this ADR adds **no** new §4 row.

2. **The `IDisplay` HAL seam is pixel-only today** (`fillScreen`/`drawPixel`/`present`/`readCanvas`),
   by deliberate KISS — its own header says it "grows only when a surface needs more". A HUD needs
   text. So the question is *where* text rendering enters the system, and how the policy stays
   host-testable when its output is glyphs on a panel that no unit test can assert.

## Decision

1. **A passive `surface/ScreenToastSurface : EventSink`, pure, mirroring `LedStatusSurface`.**
   `onAppEvent` updates an in-RAM model and never reads a clock or touches hardware; `tick(nowMs)` does
   every time-based decision (toast expiry, the liveness heartbeat) and renders. Time enters only
   through `tick`, so the whole policy is host-tested on the native lane against a fake, as the LED and
   the engine are. It is subscribed to the bus and ticked after the supervisor in `huntLoopPump`,
   beside the LED.

2. **The policy emits a pure `ScreenView` view-model and pushes it to an abstract `ScreenRenderer`
   seam, render-on-change — it does no layout.** `ScreenView` names *what* to show (status enum,
   cumulative counters, last-sync result, a liveness flag, and an optional active toast carrying the
   ESSID + BSSID); it carries no pixel coordinates, fonts, or colours. The device-only
   `Esp32ScreenRenderer` turns a `ScreenView` into glyphs on the panel. This is the same split the LED
   uses (`LedStatusSurface` computes a `LedStatus`; `Esp32LedDriver` renders it), and it is what keeps
   the policy testable: a `FakeScreenRenderer` records the last `ScreenView`, and the host tests assert
   the *model* a sequence of events + clock produces — behaviour, not pixels. The surface renders only
   when the view changes (compared field-by-field), so the panel sees discrete updates, not a per-tick
   redraw stream.

   *Rejected: the policy drawing text directly through `IDisplay`.* It would couple the policy to pixel
   geometry and font metrics, and force the host tests to assert `(x, y)` draw calls — implementation
   internals, exactly what the quality bar forbids. The view-model seam is the reason this surface is
   as host-tested as the LED despite rendering far more.

3. **`IDisplay` gains one text primitive; it stays the single panel owner.** `drawText(x, y, utf8,
   color565, size)` is added to the seam — the LGFX implementation forwards to the sprite's
   `drawString`, and `NullDisplay` no-ops it, so a screenless board still compiles no `LGFX_*` class
   (ADR-0002 #4). The `Esp32ScreenRenderer` holds the *same* `IDisplay&` the bring-up frame and the
   serial `dump` already use (the singleton in `main.cpp`, passed into `huntLoopBegin`), so there is one
   canvas and one owner — never a second panel seam competing for the sprite. This is precisely the
   "grows only when a surface needs more" the `IDisplay` header anticipated; the toast is that need.

4. **The HUD shows only bus-derivable facts (invariant #13); the toast is a banner over the HUD.**
   From `DrainStarted`/`DrainCompleted` it derives the status (Working while off-air; then
   Hunting/Degraded/Fault by the same reading `LedStatusSurface::statusFromDrain` uses) and a cumulative
   *uploaded* count (`accepted + duplicate`, the terminal upload successes in `DrainOutcome`). It counts
   `NewPassword` facts as *cracks*, and reads the last `SyncOutcome` for the sync line. It shows **no**
   capture-queue depth, because the bus carries no queue-depth fact and a surface must not reach into
   engine state to invent one (#13). A `NewPassword` also arms a transient **CRACKED** banner naming the
   ESSID + BSSID, drawn over the lower part of the persistent HUD for a fixed hold, then cleared back —
   the screen analogue of the LED's recovered flash. The banner does **not** carry the plaintext PSK:
   the surface copies only the ESSID + BSSID out of the `CrackedResult`, never `.password` (mirroring
   ADR-0023 decision 4, and honouring ADR-0006's secret-at-rest treatment of a recovered key).

5. **Rendering is proven by the canvas-dump screenshot, the policy by host tests.** Glyph legibility,
   layout, colour, and orientation cannot be asserted in a unit test — that is the case docs/quality-bar
   §3 exists for. So the *rendering* is proven the way the bring-up frame is: the bench probe drives a
   real board, the serial `dump` streams the sprite, and `scripts/0026-screen-verify.mjs` reconstructs a
   PNG for human review (reusing slice-0005's dump→PNG path wholesale). The bench probe re-arms the toast
   so a single frame captures the HUD *and* the banner together, proving the whole render path in one
   artifact. The *timing* the screenshot cannot show — that the toast arms on a crack and clears after
   its hold, that counters accumulate, that a status change moves the HUD — is proven on the native lane
   against the fake renderer, where a fake clock makes it deterministic.

## Consequences

- One more bus subscriber (LED, sync-logger, optional webhook, now the screen = 4 of `kMaxSinks = 6`,
  the "toast" slot ADR-0021 already reserved). No new §4 invariant: a passive display is exactly what
  invariant #13 was written for.
- `huntLoopBegin` gains an optional `IDisplay*` (defaulted null). `main.cpp` passes the shared
  `display()`; the surface is wired only when a display is passed *and* the active board has a panel, so
  the existing probes (led/webhook/sync/upload) and screenless boards are unaffected. The screen bench
  probe passes it to exercise the render path.
- The font glyphs and the text code cost flash. The measured cost is recorded in the slice's `As built`
  from the board compile, not estimated here (§4: cite measured data).
- The view-model→renderer split is now used by two surfaces (LED, screen). A future third surface that
  renders a computed view (the web dashboard) is the rule-of-three trigger to consider extracting a
  shared "surface renders a view-model" abstraction; until then each stays concrete (quality bar: reuse
  on the third instance, not the first).
- `statusFromDrain` now has two near-identical readers (the LED maps a drain to `LedStatus`, the screen
  to `ScreenStatus`). This is the *second* instance, not the third, so the mapping is deliberately left
  duplicated rather than hoisted into a shared status enum the LED and screen would both depend on;
  recorded here so a later reader does not "fix" the duplication prematurely (LEDGER carries the note).
