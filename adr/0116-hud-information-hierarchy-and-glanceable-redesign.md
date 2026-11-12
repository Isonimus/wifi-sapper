---
id: '0116'
title: "HUD information hierarchy: persistent bands, a one-row-each capture target, and glance-grade data"
type: architecture
status: accepted
date: 2026-11-12
supersedes: []
superseded_by: []
---

## Context

The hunt HUD grew field by field: the live-hunt line (ADR-0033), the battery icon
(ADR-0091), the two-tier layout (slice-0088), and the two device renderers
(ADR-0084/ADR-0086). Each addition stacked another equal-weight, left-aligned text
row. Nobody ever decided an *information hierarchy*; the result reads as seven rows
of equal weight, which a one-second glance parses slowly.

It also has a concrete rendering bug on the **compact tier** (240x135: Cardputer,
M5StickC Plus2). For lack of vertical room there, the capturing target label and the
Beacon/M1-M4 indicator row are crammed onto one shared line (`huntIndY == huntY`),
and the label is clamped only to its 19-char buffer, not to the pixels left before
the indicators (`screen_renderer_esp32.cpp`). A formatted BSSID
`AA:BB:CC:DD:EE:FF` is 17 chars = 102px from margin 4, while the indicator row starts
at x=88 (column 14), so `:FF` paints under the `B/1/2/3/4` glyphs: garbage. A long
SSID collides the same way. The roomy T-embed tier and the CYD direct-panel renderer
give the indicator row its own line and are immune, which both localizes the bug and
exposes its root: two variable-width elements (a target name up to 32 chars, or a
17-char MAC, AND a fixed five-slot cluster) were placed on one row, and two greedy
elements on one row eventually touch.

Two independent design passes (the maintainer's, and a from-scratch review blind to
the code) converged on the same conclusion: this is a **layout-category error, not a
sizing one**. Give the identity and the progress their own full-width rows and the
collision is impossible by construction, a 32-char SSID is 192px and a 17-char MAC
102px, both inside the 232px usable width, so no truncate-to-fit heuristic is ever
needed.

Beyond the bug, a glance wants two data the HUD omits on a device the operator is
physically standing beside:

- the **target's signal strength** — we already record discovery-time RSSI
  (ADR-0068, §4 #31) but never surface it, yet it tells the operator at a glance
  whether this window is likely to pay off;
- **whether the unit is actively transmitting** — deauth (ADR-0029) and clientless
  PMKID solicitation (ADR-0110) are operator-armed behaviours the operator is legally
  and ethically responsible for (ADR-0107 consequence, README), and the HUD today
  gives no sign that either is on the air.

## Decision

Define the hunt HUD as a stack of **persistent bands with fixed roles**, redesigned
for a one-second glance. Exact coordinates, fonts, and palette stay photo-tuned
candidates per board (ADR-0002 §5, slice-0088's tier mechanism); this ADR fixes the
*information rules*, not the numbers.

1. **Four bands, fixed roles; only the body swaps.** Header (name + version +
   battery + the armed-TX indicator), Status (the phase word + liveness + target
   signal), Body (mode-dependent: the SCAN line vs the capture target), Footer (the
   reference counters + sync). The three framing bands keep their role across both
   hunt modes so the eye learns where to look before the screen updates; only the
   body changes with `HuntPhase`.

2. **The status word is health/phase, not activity.** The status word
   (`ScreenStatus`, ADR-0025) answers "is the loop alive and OK"
   (HUNTING/WORKING/DEGRADED/FAULT + the heartbeat); the body answers "what is it on
   right now" (`HuntPhase`, ADR-0033). These are already two separate fields and must
   not be collapsed into one word.

3. **Identity and progress never share a row (the collision-proof rule).** In the
   capturing body, the target identity (SSID, or the BSSID when the SSID is hidden)
   occupies its own full-width row, and the handshake-progress indicator row occupies
   its own full-width row below it. No renderer may co-locate them, and none needs a
   truncate-to-fit heuristic for the stated maxima. This is the structural fix the
   collision bug demands, and it binds every renderer (compact, roomy, direct-panel).

4. **Indicators are labelled B / M1 / M2 / M3 / M4, and PMKID is a distinct marker,
   not a sixth slot.** The five indicators name Beacon + the four-way messages
   (matching `hasM1..hasM4` and the ADRs), replacing the bare `1/2/3/4`. A clientless
   PMKID is **not** a sixth frame of the four-way sequence: it is an alternative
   terminal condition extracted from M1 (`isPmkidValid`, ADR-0110 / §4 #47). It is
   therefore rendered as a **separate `PMKID` badge, set visually apart from the
   B/M1..M4 row** and lit only on the clientless path, never appended to that
   progression, so the four-way row stays a pure four-way ladder and the alternative
   terminal reads as the distinct thing it is. (A highlight on the M1 cell was
   considered and rejected: it visually ties the PMKID to one cell of the sequence it
   is not part of.)

5. **Signal strength is a coarse bucket compared as the bucket, never raw dBm text.**
   The capturing target's RSSI is shown as a small multi-bar glyph driven by a coarse
   bucket, exactly the discipline the battery gauge already uses (ADR-0091, §4 #40):
   raw dBm dither must not reach the `ScreenView` equality (ADR-0025 decision 2) or it
   forces a redraw every sample. The datum is the discovery-time RSSI the registry
   already records (ADR-0068), threaded engine -> `HuntSnapshot` -> `ScreenView` as a
   bucket, with `ScreenView::operator==` comparing the bucket. RSSI as text is
   illegible at glance distance anyway.

6. **Armed-TX state is a persistent indicator the event banner must never occlude.**
   Whether deauth (ADR-0029) and/or PMKID solicitation (ADR-0110) are armed is shown
   as a persistent header indicator whose *presence of colour* (not the letter)
   carries "this unit is transmitting, and I am responsible." Because it is
   safety-bearing, the CAPTURED/CRACKED banner (ADR-0031) is constrained to overlay
   the **body region only** and must never cover the header or status bands: a safety
   indicator a congratulatory popup can hide is one the operator cannot trust. This
   refines ADR-0031's banner (which overpainted the whole lower HUD) without
   superseding its one-slot/two-label decision.

7. **Progressive disclosure for news-only data.** A datum that is interesting only
   when nonzero is hidden when zero, so the normal state stays quiet and a nonzero
   value is itself the signal; reference data (the firmware version) stays small and
   dim. The motivating case is a pending-upload-queue count, but it needs a new
   cosmetic pull seam (§4 #13 keeps the capture->queue path off the EventBus), so it
   is deferred (LEDGER), not mandated here.

This supersedes nothing: slice-0088's frozen account of the two-tier layout stays
true for its date, ADR-0033's live-line seam and ADR-0025's render-on-change contract
are reused unchanged, and ADR-0084/ADR-0086's two-renderer split is the thing this
redesign is applied to (twice), not replaced.

## Consequences

- **Two renderers change, not one.** `Esp32ScreenRenderer` (compact + roomy tiers)
  and `DirectPanelScreenRenderer` (CYD), which ADR-0086 deliberately keeps as two
  instances (§3 rule-of-three; a shared helper still waits for a third renderer). The
  pure `ScreenToastSurface`/`ScreenView` policy changes once; the device draw code
  changes twice.
- **`HuntSnapshot` gains two small fields** — a bucketed RSSI, and a `hasPmkid` bool
  the engine computes live off the M1 it already holds (via the pure
  `extractPmkidFromEapol`, so §4 #8/#47's no-EAPOL-parsing-outside-the-pure-core
  holds). `ScreenView` gains the RSSI bucket, the PMKID marker, and the two armed-TX
  booleans (static from the provisioning record, known at `huntLoopBegin`). Each new
  `ScreenView` field must join `operator==`, or render-on-change silently breaks.
- **It reopens the eyes-on HUD proof.** This redesigns a photographically-verified
  device surface, so it re-owes the on-air HUD photo on the Cardputer and the
  M5StickC Plus2 (sprite boards, §4 #22/#39) and the CYD (§4 #38/#39): operator
  hardware time, tracked in the LEDGER. ADR-0086 decision 1's "sprite boards ship with
  no re-verification" held only while the renderer was unchanged; this changes it
  deliberately.
- **Shipped in two stages** (two LEDGER items): first a minimal collision fix
  (indicators to their own row, relabelled B/M1..M4, label clamped to the available
  width as a belt-and-braces guard) so the two panels stop mangling immediately; then
  the full hierarchy + RSSI + armed-TX + PMKID-marker redesign. The staging keeps the
  urgent correctness fix off the critical path of the larger visual work.
- The exact band coordinates, fonts, and palette (off-white over a dark field, muted
  dividers, dimmed MAC colons so an address reads as six octets, one accent per band)
  are photo-tuned candidates on the panels (ADR-0002 §5), not fixed here. The band
  *roles* and the seven rules above are the invariant a future reader must not
  "simplify" away, in particular re-merging the identity and indicator rows, appending
  PMKID as a sixth slot, letting raw dBm into the view, or letting the banner cover the
  armed-TX indicator.
