---
id: '0054'
title: "Maintenance entry: a windowed post-boot button poll that actually works on the Cardputer"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0053: make Maintenance reachable on the reference Cardputer (and any board whose entry
button is the GPIO0 strapping pin) by replacing the impossible "hold BOOT at power-on" gesture — which
drops the ESP32 ROM into serial download mode instead of running firmware — with a debounced,
early-exit poll of the per-board `bootButtonPin` during a bounded window *after* a normal boot.

- **Pure unit (`config/entry_gesture.h`):** a `PressDebouncer` state machine + the named constants
  (`kMaintenanceEntryWindowMs`, `kEntryPollIntervalMs`, `kEntryDebounceSamples`); host-tested.
- **Device seam (`main.cpp` `maintenanceRequested()`):** boot normally, then poll the button for the
  window, feeding `PressDebouncer`; first confirmed press → Maintenance (early-exit), else `false`.
  Evaluated as `hasCreds && maintenanceRequested()` so an unprovisioned boot pays no dwell. The
  `SAPPER_TEST_HOOKS` forced-entry short-circuit is unchanged.
- **Docs:** `CLAUDE.md` §4 #21 updated to cite ADR-0053 and the windowed press; README + the
  setup-form help text + the BOOT-hold code comments re-worded from "hold at power-on" to "press
  during the boot splash".
- **LEDGER:** the defect (raised when found) is resolved here rather than deferred, so no standing
  item remains; the slice-8 item gains the richer-gesture follow-up.

## Definition of Done

**Scenario A — an isolated glitch never enters Maintenance, but a real press does (pure).**
- **Given** a `PressDebouncer(kEntryDebounceSamples)` fed a stream of active-low samples
- **When** the stream contains a lone LOW between HIGHs, then later `kEntryDebounceSamples` consecutive LOWs
- **Then** `feed()` returns `false` through the lone glitch (the run resets on the HIGH) and returns
  `true` only once the consecutive run reaches the threshold.
- **Proof:** `test/test_entry_gesture` — fails against the old single-sample behaviour (which had no
  debounce: the first LOW, glitch or not, entered Maintenance). See `## As built`.

**Scenario B — a normal boot is not routed to Maintenance and an unprovisioned boot pays no dwell (review).**
- **Given** a provisioned device booted with no BOOT press, and separately an unprovisioned device
- **When** `setup()` evaluates `hasCreds && maintenanceRequested()` and calls `decideBootPhase()`
- **Then** the provisioned no-press boot proceeds to Station after the window; the unprovisioned boot
  routes to Provisioning **without** running the window poll at all (short-circuit on `!hasCreds`).
- **Proof:** `decideBootPhase()` precedence unchanged and covered by `test/test_provisioning`; the
  `hasCreds &&` short-circuit and the poll wiring are review-only glue (as the single sample was).

**Scenario C — forced entry still drives the on-device Maintenance verify (tooling).**
- **Given** a `SAPPER_TEST_HOOKS` build with `SAPPER_TEST_MAINT` set
- **When** `maintenanceRequested()` runs
- **Then** it short-circuits to `true` before the poll, so `verify:maintenance` reaches entry→AP→serve
  exactly as before (a physical press cannot be driven headlessly).
- **Proof:** `scripts/0040-maintenance-verify.mjs` (unchanged) still passes. See `## As built`.

## Verification

- **Pure debounce (Scenario A)** — `pio test -e native` (`test_entry_gesture`), fail-before/pass-after
  recorded in `## As built`.
- **Boot-gate routing (Scenario B)** — `test/test_provisioning` (`decideBootPhase`, unchanged); the
  glue is review-only, as ADR-0053 decision 2 records.
- **Forced-entry verify (Scenario C)** — `scripts/0040-maintenance-verify.mjs`, unchanged; the hook
  bypasses the window. The *physical windowed gesture* is inherently manual/review-only (no CI can
  press a button), exactly as the old hold gesture was — called out here so the gap is not mistaken for
  a missing proof.

## As built

Shipped as designed (ADR-0053), in one commit with the ADR, the §4 #21 update, and the LEDGER fold-in.

- **Pure debounce (Scenario A).** `config/entry_gesture.h` holds `PressDebouncer` (a one-counter state
  machine: a pressed sample extends the run, a released sample resets it, confirms at
  `kEntryDebounceSamples` consecutive) plus the three named constants (`kMaintenanceEntryWindowMs = 3000`,
  `kEntryPollIntervalMs = 10`, `kEntryDebounceSamples = 3`). Header-only and `constexpr`, so
  `test/test_entry_gesture` covers it with no build_src_filter entry. Fail-before/pass-after: reverting
  `feed()` to the superseded single-sample semantics (`return pressed;`) made the glitch/threshold/reset
  tests **FAIL** (a lone LOW confirmed — the exact ADR-0039-decision-2 bug); the real debounce makes them
  **pass** (271 native cases total, up from 267).
- **Device seam (Scenario B).** `maintenanceRequested()` (`main.cpp`) now polls `bootButtonPin` after a
  normal boot for `kMaintenanceEntryWindowMs`, feeding each `digitalRead(pin) == LOW` sample to the
  debouncer and early-exiting on the first confirmed press; the deadline compare is millis()-wrap-safe.
  The call site is gated `hasCreds && maintenanceRequested()`, so an unprovisioned boot pays no dwell.
  `decideBootPhase()` is unchanged (test/test_provisioning still green); the poll wiring is review-only
  glue, as the single sample was.
- **Forced-entry verify (Scenario C).** The `SAPPER_TEST_HOOKS` / `SAPPER_TEST_MAINT` short-circuit is
  untouched and precedes the poll, so `scripts/0040-maintenance-verify.mjs` reaches entry→AP→serve
  exactly as before. The physical windowed press is inherently manual (no CI can press a button) — the
  same review-only gap the hold gesture had.
- **Docs & wiring.** `CLAUDE.md` §4 #21 updated to cite ADR-0053 and the windowed press; README's
  Maintenance section rewritten (press-during-splash, with the explicit "do not hold BOOT at power-on →
  download mode" warning); the setup-form help text and the BOOT-hold comments across `main.cpp`,
  `provisioning.h/.cpp`, `board_profile.h`, `cardputer.h`, `dashboard.cpp`, `maintenance_portal.h`
  re-worded. The field-reported defect is fixed here so it carries no standing LEDGER item; the slice-8
  item gained the richer-gesture follow-up.
- **Builds.** `pio test -e native` 271/271; `pio run -e cardputer` SUCCESS; `npm run lint` 0/0 (54 docs).
