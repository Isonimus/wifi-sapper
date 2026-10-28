---
id: '0093'
title: "Inactivity screen dimming is backlight-only and event-driven"
type: architecture
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Context

The Sapper is a battery field appliance (the reason slice-0092 added a battery indicator), and its
one valuable activity, the endless hunt->capture->upload->sync loop, runs entirely from `loop()`
(`huntLoopPump`). The panel backlight is the single largest avoidable power draw on a deployed unit,
and today it stays at full brightness the whole time the device hunts, even though nobody is watching
the HUD on an autonomous planted unit. Dimming the backlight directly extends the field runtime the
battery indicator measures, and a dark screen on a planted unit is also an OPSEC win.

The parent project solved this for its interactive handheld (`adversary:ADR-0116`, a two-stage
backlight dim superseding its one-shot `adversary:ADR-0114`). Two things from that decision carry
over unchanged, and one must change for this appliance.

Carries over, and is the reason this ADR exists:

- **The CPU must never sleep.** The obvious "screen sleep" primitives on ESP32 are
  `esp_light_sleep_start()` / `esp_deep_sleep_start()`, and both halt `loop()`. On the Sapper that
  does not pause an idle UI, it stops the hunt: no promiscuous capture, no queued upload, no hourly
  sync. A unit left "asleep" in the field would silently collect nothing, the exact failure the
  appliance exists to avoid. So the design question is not "how do we sleep" but "how do we save power
  without ever stopping the loop", and the answer has to be written down because a later
  power-optimization pass would otherwise reach for precisely the CPU-sleep call this decision forbids.
- A **two-stage** curve is better than one-shot: a readable intermediate (counts still glanceable,
  most of the draw saved) before full black (maximum saving + OPSEC).

Must change: the parent is **interactive**, so there "activity" is a key/gesture and the screen wakes
on input. The Sapper is **headless-first and autonomous**: during a hunt there is no routine input,
and the HUD is an event display fed by the bus. So "activity" here is redefined to be **a notable bus
event** (a capture or a crack), with input wake kept only where a board actually has a pollable input.
This makes the HUD meaningful (it brightens when the unit catches something, then goes dark) instead
of a screen that is either always-on or always-dark.

## Decision

1. **Backlight only; the hunt loop never pauses.** Inactivity dimming changes only the panel backlight
   (`IDisplay::setBrightness`), nothing else. `esp_light_sleep`, `esp_deep_sleep`, and any other CPU,
   clock, or scheduler gating that would pause `loop()`/`huntLoopPump` are **forbidden** in the dimming
   path. The dimmed and off states are cosmetic; capture, upload, and sync keep running in the dark.
   This invariant binds all future work, not just this slice (new standing invariant, §4).

2. **Two stages, from a pure predicate.** A pure `inactivityStage(nowMs, lastActivityMs, dimAfterMs,
   offAfterMs) -> {Active, Dimmed, Off}`, evaluated once per hunt-loop tick, drives the backlight:
   Active = full, Dimmed = a fixed readable level (`kScreenDimBrightness`, ~15% of full), Off = 0.
   `dimAfterMs == 0` disables the feature entirely (Active always); `offAfterMs == 0` is dim-only
   (never fully off). The predicate is wrap-safe over `millis()` (elapsed = `nowMs - lastActivityMs`)
   and robust to an out-of-order config (Off wins once `offAfterMs` is passed, even if a hand-edited
   config set it below `dimAfterMs`), so it is host-tested on the native lane like every other policy.

3. **Activity is a bus event, plus input where a board has it.** The hunt-phase screen's last-activity
   timestamp is reset by a notable bus fact (the toast-worthy `HandshakeCaptured` / `NewPassword`, the
   same set that raises a banner; routine hourly `SyncCompleted` is excluded so a sync does not keep
   the screen lit). On a board with a pollable input (the Cardputer keyboard, a button board's side
   button) any input also resets it, so an operator can wake the HUD by hand; a screenless/input-less
   board simply never gets an input wake. A wake transition only restores brightness; it routes
   nowhere (there is no UI to route to in the hunt loop), so unlike the interactive parent there is no
   wake-swallow to reason about.

4. **`IDisplay` gains `setBrightness(uint8_t)`.** The seam grows by one method (KISS: it grows when a
   surface needs it). `LgfxDisplay` and `DirectPanelDisplay` forward to the LovyanGFX panel's
   `setBrightness`; `NullDisplay` no-ops it (a screenless board dims nothing). The dimming controller
   holds an `IDisplay&` and is host-tested against a fake display that records the brightness calls.

5. **Configured through the provisioning portal, off by default.** Two fields,
   `screenDimAfterSec` and `screenOffAfterSec`, join `ProvisioningRecord` (NVS-backed like the deauth
   arm and the notify selector), set from two number inputs on the captive-portal setup form, both
   defaulting to 0 (feature off), so a fresh or pre-existing device is unchanged until the operator
   opts in. The inputs are **prefilled with the stored value** (they are config, not secrets, so echoing
   the value is safe, unlike the secret fields which only ever carry a stored-flag), so a re-save to
   change something else preserves them rather than wiping them to 0, matching how the checkboxes preserve
   their state. The field is parsed server-side and **clamped to the u16 range** (the HTML `max` is
   advisory and a raw POST could otherwise wrap a cast), and `screenOffAfterSec`, when nonzero, is clamped
   `>= screenDimAfterSec` on save so the readable stage is never skipped; the pure predicate is safe even
   if either clamp is bypassed. The delays are read once at `huntLoopBegin` and seconds are converted to
   the millis the predicate uses.

This **extends** ADR-0002 (a board capability, the backlight, driven through the HAL seam), ADR-0025
(the HUD is a bus observer), and ADR-0006 (a new provisioned setting through the one NVS seam). It
adapts `adversary:ADR-0116` to this appliance and **supersedes none**: no earlier Sapper decision
addressed backlight power.

## Consequences

- **A new §4 standing invariant:** inactivity screen dimming is backlight-only and never sleeps the
  CPU or otherwise pauses the hunt loop. The row is added in this commit. A future power pass that
  reaches for `esp_light_sleep`/`esp_deep_sleep` in the dim path violates it.
- `IDisplay` grows `setBrightness(uint8_t)`; the three real/null impls implement it. The dimming
  controller + the pure `inactivityStage` predicate live in `surface/` and compile on the native lane.
- `ProvisioningRecord` gains `screenDimAfterSec` / `screenOffAfterSec`; the setup form, the
  provisioning store (absent-key default 0 = off, behaviour-preserving), and `resolveProvisioningUpdate`
  (pair-and-toggles-from-submission, plus the off>=dim clamp) grow to carry them. `README.md`'s
  provisioning section documents the two new rows.
- Verification: the pure predicate and the controller (against a fake display + fake clock) are native
  tests run in CI; the on-device half is a verify that drives the hunt loop and asserts the backlight
  transitions are logged (`[DIM]` stage line) with no CPU-sleep, the physical dimming being the
  photographic half.
- Scoped to the hunt-phase HUD. Dimming the Maintenance-phase screen (a different loop) is a possible
  later extension, recorded in `LEDGER.md`.
