---
id: '0053'
title: "Maintenance entry is a windowed post-boot poll of a per-board button, not a hold-at-power-on"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

ADR-0039 decision 2 enters Maintenance by **holding BOOT at power-on**: `maintenanceRequested()`
(`main.cpp`) reads the board's `bootButtonPin` — GPIO0 on the reference Cardputer
(`config/boards/cardputer.h`) — once, immediately in `setup()`, and a LOW reading routes
`decideBootPhase()` to Maintenance.

**That gesture is physically impossible on the reference hardware, and on nearly every ESP32
board.** GPIO0 is the ESP32/ESP32-S3 **strapping pin**: the ROM samples it at reset-release, and a
LOW there puts the chip into serial *download* mode — the application firmware never runs, so
`setup()` never executes and the pin is never sampled. To read GPIO0 LOW at the current sample site
you must have held BOOT through reset, which means you are in the bootloader, not the firmware. The
documented "hold BOOT at power-on" gesture therefore opens the ROM download stub, never Maintenance.
The board profile even flagged GPIO0 as "the one pin where an accidental zero would be behavioural"
(`board_profile.h`) — but treated that as a spurious-entry hazard, missing that it makes the
*intended* entry unreachable. Reproduced in the field on the Cardputer; raised as a LEDGER `[defect]`
when found and resolved in this change rather than deferred, so it leaves no standing LEDGER item.

The single early sample cannot be salvaged by "press BOOT after boot" either: it fires once, ~a few
ms into `setup()`, so an operator would have to already be holding the button at that instant — the
same hold-through-reset that triggers download mode. The escape hatch is that **GPIO0 read *after*
boot is an ordinary input** (strapping only matters at reset-release), so a device that boots
*normally* (BOOT not held) can poll the button afterward and read it fine.

This is the `~/.claude/CLAUDE.md` §2 case: ADR-0039's boot-gate *precedence logic* (pure,
host-tested in `decideBootPhase()`) was correct and stands; its *entry gesture* was wrong on the
hardware it targets. Recorded as a decision that supersedes only that clause — the ADR-0027→ADR-0029
precedent for decision-level supersession — not a silent patch, and not a wholesale retirement of
ADR-0039 (whose dashboard, PSK-serving, SoftAP-hardening, panel, backstop, and control decisions are
untouched and still hold).

## Decision

**Maintenance is entered by pressing the per-board entry button during a bounded window *after* a
normal boot — never by holding it through reset. `decideBootPhase()` and its precedence are
unchanged; only the device seam that produces its `maintenanceRequested` input changes.**

1. **Windowed post-boot poll, not a single sample.** After the splash presents, `maintenanceRequested()`
   polls `bootButtonPin` for `kMaintenanceEntryWindowMs` (3 s). The operator powers on *normally*
   (BOOT not held → firmware runs), then presses BOOT once during the window. The first **debounced**
   press wins and returns immediately (early-exit into Maintenance); if the window elapses with no
   press, it returns `false` and boot proceeds to Station/Provisioning as before. A deliberate entry
   is fast (reaction time + debounce); only a normal boot pays the full window.

2. **Debounce, because a spurious entry has a security cost.** A press is confirmed only after
   `kEntryDebounceSamples` (3) *consecutive* LOW samples at `kEntryPollIntervalMs` (10 ms) cadence
   — a released (HIGH) sample resets the run — so an isolated electrical glitch on the strapping pin
   never confirms. This matters because Maintenance serves recovered plaintext PSKs (ADR-0039
   decision 5): an accidental entry is a mild disclosure hazard, not merely a cosmetic blip. The
   debounce state machine (`PressDebouncer`, `config/entry_gesture.h`) is **pure and host-tested**;
   the millis-bounded poll loop that feeds it real samples is the device glue (review-only, as the
   single sample was).

3. **The entry input stays a per-board declaration — the existing `bootButtonPin` seam, unchanged.**
   Core code polls whatever pin the board declares (ADR-0002: branch on what a board *has*). The
   Cardputer keeps `bootButtonPin = 0` (GPIO0/BOOT), now polled *after* boot rather than sampled at
   it. A board whose entry button is **not** a strapping pin (e.g. an M5Stick side button) declares
   that pin instead and, because it is not strapped at reset, may even be held at power-on — the
   windowed poll subsumes that case without special-casing. `bootButtonPin = -1` still means "no
   entry button; never enters Maintenance from here."

4. **No richer per-board input hardware is brought up for this.** Reading a Cardputer *keyboard* key
   would require standing up an I2C keyboard driver before Wi-Fi — a driver that does not exist yet
   (`InputKind::Keyboard` is declared but unread). The GPIO poll fixes the defect on every board that
   already has a BOOT button (the same broad set the old gesture targeted) with zero new drivers, so
   building a general input-init framework now would be speculative generality (§3 KISS / rule of
   three). Richer per-board entry gestures (a keyboard key, touch) are deferred to slice-8, where
   those input drivers get built for their own reasons and a second real case justifies the
   abstraction (LEDGER).

5. **The shipped boot face is unchanged — §4 #22 holds.** The window is a silent dwell over the
   existing splash; no Maintenance prompt is drawn, so ADR-0041's "boot face renders only the product
   splash" is not touched. The gesture is documented instead (README, the setup-form help text) —
   which is required regardless, since headless boards (no panel) have no on-screen affordance to rely
   on. An on-screen prompt would be a separate change that reopens ADR-0041; it is deliberately not
   made here.

6. **The window runs only on a provisioned device.** `maintenanceRequested()` is evaluated as
   `hasCreds && maintenanceRequested()`, so an unprovisioned first boot (which `decideBootPhase()`
   routes to Provisioning regardless of the button) pays *no* entry dwell. This is behaviour-preserving
   — `decideBootPhase()` already ignores the maintenance signal when unprovisioned — and removes the
   only boot the 3 s window would otherwise slow needlessly.

7. **The bounded window is not an ADR-0003 #3 "blocking state."** §4 #3 requires states that block
   *indefinitely* before the engine loop (the captive portal, Maintenance itself) to pump the serial
   channel so they stay observable. The entry window is a bounded ≤3 s boot dwell, not an interactive
   state, and — like the existing splash present and the pre-existing single sample — does not pump
   serial. Recorded here so the omission is a decision, not an oversight a reviewer re-flags.

8. **The `SAPPER_TEST_HOOKS` forced-entry path is unchanged.** `SAPPER_TEST_MAINT` still short-circuits
   `maintenanceRequested()` to `true` before the poll (a physical press cannot be driven headlessly,
   ADR-0039 decision 9 / §4 #4-style hook), so `scripts/0040-maintenance-verify.mjs` drives
   entry→AP→serve exactly as before. The windowed *physical gesture* is inherently review-only /
   manual (no CI can press a button), as the hold gesture was.

`kMaintenanceEntryWindowMs = 3000` is a UX constant, not a measurement: long enough to react to the
splash, short enough not to burden a rare boot on an endless-hunt appliance. It carries no probe; if
field use shows it too tight or too slow, amend this ADR with the observed value.

## Consequences

- **Supersedes ADR-0039 decision 2 only** (the "hold BOOT at power-on" entry gesture). ADR-0039 stays
  `accepted`; its decisions 1, 3–11 are unchanged and still govern. Frontmatter `supersedes` is left
  empty by design — this is decision-level supersession recorded in prose and in the §4 table, per the
  ADR-0027→ADR-0029 precedent, because whole-ADR supersession would wrongly retire the still-correct
  rest of ADR-0039.
- **§4 invariant #21 is updated in this commit** to cite this ADR and describe the windowed post-boot
  press (replacing "a BOOT/GPIO0 hold … at power-on"). The rest of #21 (hunt-suspended phase,
  persisted-state reads, PSK exception) is unchanged.
- **Live docs updated in the same change** (§1): README's Maintenance section, the setup-form help
  text (`provisioning_form.cpp`), and the BOOT-hold comments across `main.cpp`, `provisioning.h/.cpp`,
  `board_profile.h`, `cardputer.h`, `dashboard.cpp`, `maintenance_portal.h` now say "press BOOT during
  the boot splash," not "hold BOOT at power-on."
- **New pure unit + test:** `config/entry_gesture.h` (`PressDebouncer` + the three named constants),
  covered by `test/test_entry_gesture` on the native lane — an isolated glitch never confirms, N
  consecutive LOWs do. The old single-sample seam had no logic to test; this one does.
- **Boot-time cost:** a provisioned device with no press waits `kMaintenanceEntryWindowMs` (~3 s) at
  cold boot before hunting. Negligible on an appliance that boots rarely; an intentional entry
  early-exits well under that. Unprovisioned boots are unaffected (decision 6).
- **Extends ADR-0002** (the entry input stays a per-board profile fact) and leaves ADR-0041 (#22
  boot face) and ADR-0003 (#3 blocking states) untouched by explicit decision (5, 7). No other ADR's
  reasoning changed.
- The defect was raised as a LEDGER `[defect]` when found, then resolved in this change rather than
  deferred, so it carries no standing LEDGER item (found and fixed together — the ledger tracks *open*
  work). The slice-8 hardware-broadening item gains the richer-per-board-gesture follow-up (decision 4).
