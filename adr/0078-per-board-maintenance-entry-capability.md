---
id: '0078'
title: "Per-board Maintenance entry: a compile-time gesture seam selected by InputKind"
type: architecture
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Context

ADR-0057 decision 7 deferred one thing deliberately: generalising the per-board
Maintenance-entry *input* capability (button vs keyboard vs chord) into the Board Profile,
"to be decided when a second board makes the abstraction real rather than speculative." It
built the reference-board mechanism (a runtime `m` keypress on the TCA8418 that persists an
RTC reboot-intent and restarts) and left the cross-board shape open, with one hint: the
`bootButtonPin` field stays in the fact table "available to a future board that genuinely
wires a pollable non-strapping button (e.g. an M5Stick side button)."

Slice-8 brings up that second board, the **M5StickC Plus2**, and it makes the abstraction
concrete rather than speculative:

- It has **no TCA8418 keyboard**. It is an ESP32-PICO-V3-02 with three physical buttons
  (verified against M5Stack's docs: BtnA front, BtnB side, BtnC power). So the reference
  board's keyboard-`m`-press entry does not transfer at all.
- Of its three buttons, **BtnC (power) is spoken for**: M5Stack documents a >2 s long-press
  on it for power-off and a press for wake. The entry gesture must therefore live on a free
  button (BtnA or BtnB), not the power button, and must not collide with that >2 s
  power-management semantics.
- The downstream intent mechanism is **already board-agnostic** (ADR-0057 decisions 3 and 6:
  the `RTC_NOINIT` word, `consumeMaintenanceIntent`, and the pure `decideBootPhase` gate) and
  needs no change. The *only* board-variant piece is the gesture that detects the operator's
  request, writes the intent, and reboots. Today that is a single Cardputer-specific line in
  `loop()`'s `Ready` branch: `g_keyboard.maintenanceKeyPressed()`.

So the decision this ADR owes is narrow and real: where does the gesture variation live, and
what gesture does a button board use. Three shapes were considered.

- **Fork A: a runtime virtual `MaintenanceEntrySource` interface** with a `Keyboard` and a
  `Button` implementation chosen at `begin()`. Clean OO and open to a third input kind, but it
  introduces exactly the runtime polymorphism (vtable, RTTI, a heap or static-dispatch
  instance) that ADR-0002 forbids the board model: the profile is "a `constexpr`/compile-time
  constant, no RTTI, no heap, to stay within the no-PSRAM SRAM budget." A vtable for two
  implementations also runs ahead of the rule of three (global quality bar). It fights the
  grain of the established architecture.
- **Fork B: compile-time dispatch selected by the profile's `InputKind`**, the same way
  `active_board.h` already selects the active profile and the `LGFX_*` config by the single
  `SAPPER_BOARD_*` flag. Zero runtime cost, no vtable, one board's gesture compiled per build,
  and it still lifts the gesture out of `loop()` into a named seam. It is the house style:
  ADR-0002's "core code branches on what a board *has* (`profile.input`), never on what it
  *is*."
- **Fork C: an inline `else if` button poll beside the keyboard poll in `loop()`.** Smallest
  diff, but it re-scatters the entry logic across `loop()` that decision 7 asked to
  consolidate, and grows the boot god-function. Rejected.

## Decision

1. **One board-agnostic entry seam, `maintenanceEntryRequested()`**, is what `loop()`'s
   `Ready` branch polls, replacing the direct `g_keyboard.maintenanceKeyPressed()` call. Its
   per-board body is selected **at compile time by `kActiveBoard.input`** (Fork B), through the
   same `SAPPER_BOARD_*`-driven mechanism `active_board.h` uses for the profile and display
   config. No runtime polymorphism, no vtable, no heap: ADR-0002's constexpr board model is
   preserved. The seam returns `true` at most once per gesture; `loop()` then persists the
   reboot-intent and restarts exactly as it does today (the ADR-0057 intent path is unchanged).

2. **Each board's gesture keeps ADR-0057's pure/impure split** (decision 6): a pure,
   host-tested decision plus thin device-only glue.
   - **Keyboard board (Cardputer ADV):** the existing pure `isMaintenanceKey` predicate plus
     the `Tca8418Keyboard` I2C glue, unchanged. This ADR only moves the call site behind the
     seam; it does not touch the keyboard path's logic or coverage.
   - **Button board (M5StickC Plus2 and future button boards):** a **new pure hold decision**
     `decideButtonHold(pressedNow, nowMs, HoldState&)` plus a thin `digitalRead` glue over the
     profile's `bootButtonPin`.

3. **A button enters Maintenance by a deliberate debounced *hold*, not a tap.** The gesture is
   a continuous press held for `kMaintenanceHoldMs` (an architecture-derived default of
   **2000 ms**, tuning deferred on hardware like dwell/settle/deauth-cadence/backoff). A button
   is an electrical *level*, not the discrete FIFO event a keypress is, so a momentary stray
   press during the endless hunt must not trigger entry: Maintenance deliberately serves
   recovered plaintext PSKs on its SoftAP (§4 #21), so entry has to be an intentional act, not
   an accident. A hold also keeps the entry gesture distinct from whatever a tap of the same
   button may later mean once on-device UI is built, and it stays clear of BtnC's documented
   >2 s power-management long-press.

4. **The hold decision is a small pure state machine**, fail-safe and edge-firing:
   `decideButtonHold(pressedNow, nowMs, HoldState&) -> bool enteredThisTick`. It records the
   press-start time on a rising edge, returns `true` the single tick the continuous-hold
   duration first reaches `kMaintenanceHoldMs`, does **not** re-fire while the button stays
   held (so one hold is one entry, not a repeat), and resets on release. It is host-tested on
   the native lane (rising edge starts the timer; held-but-short does not fire; the crossing
   tick fires exactly once; still-held does not re-fire; release resets; a bounce before the
   threshold does not fire), mirroring the keyboard decode's purity so §4 #21's `verified_by`
   gains the button board's decision instead of a new review-only hole.

5. **The entry button is a Board Profile fact**, reusing the `bootButtonPin` field ADR-0057
   decision 7 reserved (active-low, `-1` = absent). It must name a **non-strapping, non-power**
   GPIO (never GPIO0, never BtnC/power). The M5StickC Plus2's exact GPIO is taken from the
   on-device-tested dragon-ball-radar board definition and **confirmed on-device by the slice's
   verify script**, not guessed from a retailer pinout, exactly as ADR-0057 decision 5 pinned
   the `m` keycode on hardware before accepting it. A board that leaves `bootButtonPin` at the
   `-1` default has no pollable entry button, which is a compile-visible absence, not a silent
   failure.

6. **Scope: `InputKind::Keyboard` and `InputKind::Buttons`.** `Touch` and chord gestures are
   **not** built: no board needs them (YAGNI, rule of three), and a touch board is a future
   case added to the same compile-time switch, not a redesign. A board whose `InputKind` has no
   gesture compiled (for example a headless `None` appliance) simply has no runtime Maintenance
   entry, which the headless appliance never had and which remains a compile-time absence per
   ADR-0002, not a runtime surprise.

## Consequences

- Adding a button board's Maintenance entry is bounded and mechanical, matching ADR-0002's
  "adding a board is one descriptor plus one flag": set `input = InputKind::Buttons` and a real
  `bootButtonPin` in the profile, and the `InputKind` switch wires the button gesture. No edit
  to `loop()`, to the RTC intent path, or to the boot gate.
- `loop()`'s `Ready` branch loses its Cardputer-specific keyboard call and gains the one
  board-agnostic seam. The entry logic decision 7 wanted consolidated now lives behind a single
  named function, host-tested on its pure half for every input kind.
- No runtime polymorphism is introduced; ADR-0002's constexpr/no-RTTI/no-heap profile model is
  preserved intact. A future `Touch` or chord entry is the next arm of the same switch with its
  own pure decision, not a new mechanism.
- The `bootButtonPin` field (carried speculatively since ADR-0057) gets its first real
  consumer, and its documented semantics (active-low, `-1` = absent) are now exercised.
- §4 #21 is updated in the same commit as this ADR to cite ADR-0078 for the per-board entry
  generalisation and to add the button-hold pure test to its `verified_by`; the keyboard path's
  existing coverage is unchanged. The `bootButtonPin` field comment that pointed at "slice-8
  (ADR-0057 decision 7)" is likewise updated to cite this ADR.
- Deferred to the ledger: the 2000 ms hold is an architecture-derived default to be tuned on
  hardware like the other cadence constants; and full TCA8418 text-entry keyboard bring-up stays
  deferred (ADR-0057) and is untouched here. This ADR is the entry gesture only, not on-device
  text input.
