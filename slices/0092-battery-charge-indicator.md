---
id: '0092'
title: "Battery charge indicator on the HUD"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Show remaining battery charge on the HUD of a screen-equipped board, so an operator can see at a glance
that a field unit is about to die before it dies silently mid-hunt. Today the `batteryAdcPin` reserved in
every `BoardProfile` (ADR-0002) is never read: the HUD shows hunt state and nothing about power. This
slice reads the pin through a new `BatteryGauge` HAL seam, interprets it in a pure `BatteryModel`, and
draws a coarse battery icon on the HUD, implementing ADR-0091.

This is item 4 of the HUD/UI feature set (after slice-0088's resolution-aware HUD + version and
slice-0090's first-boot provisioning screen), and the last of the set. It proves battery on the one board
on the bench (the M5StickC Plus2); the Cardputer and T-embed divider calibration are deferred to a
later data-only change (LEDGER), and the CYD has no gauge. It does not close the slice-8
hardware-broadening umbrella.

## Definition of Done

- **Given** the M5StickC Plus2 (a board with a measured battery divider) running on battery
- **When** the hunt HUD is up
- **Then** the panel shows a battery icon whose fill reflects the cell charge (more segments at higher
  voltage), and the serial log reports a `[BATT] mv=<cell> level=<name>` line whose cell millivolts match
  a multimeter reading of the cell within measurement tolerance.

- **Given** a board whose battery divider is uncalibrated (Cardputer, T-embed) or which has no gauge
  (CYD, `batteryAdcPin -1`), or a screenless board
- **When** it runs
- **Then** no wrong charge level is ever shown: an uncalibrated or absent gauge resolves to `Unknown`
  (the icon is hidden, or absent with the panel), the serial log reports `[BATT] disabled (no gauge)`
  where there is no gauge, and there is no `[FATAL]` and no half-drawn icon.

- **Given** a cell voltage dithering at a bucket boundary
- **When** the model is fed readings that cross and re-cross the threshold within the hysteresis band
- **Then** the reported `BatteryLevel` does not flip back and forth: it changes only once a reading
  clears the band, so the icon does not flicker between two levels.

## Design

**Existing code considered.** The seam/pure-policy split is copied from the status LED
(`led_driver.h` seam + `Esp32LedDriver` device impl under `#ifndef UNIT_TEST` + pure host-tested
`LedStatusSurface`, ADR-0021): the `BatteryGauge` seam is the LED's structural sibling for an input
peripheral instead of an output one. The injected pull-source is copied from the live-hunt HUD
(`ScreenToastSurface::setHuntSource`, ADR-0033/§4 #18): the gauge is read on tick exactly as the hunt
snapshot is, so a fake gauge host-tests the surface. The icon is drawn in the resolution-aware
`Esp32ScreenRenderer` slice-0088 produced, reusing its two-tier layout, not a new renderer. What was
deliberately **not** reused: the `DirectPanelScreenRenderer` is left untouched (its only board, the CYD,
has no gauge, so a battery draw there is speculative, §3). The `analogReadMilliVolts` calibrated read is
the raw datum, the `wifi_pkt_rx_ctrl_t.rssi` analogue of §4 #8, so no interpretation is added to the
seam.

**Seam.** `BatteryGauge` returns `{present, pinMilliVolts}`. `Esp32BatteryGauge(profile)` reads a median
of a few `analogReadMilliVolts(profile.batteryAdcPin)` samples when `batteryAdcPin >= 0`, and reports
`present = false` otherwise. Device-only, behind `#ifndef UNIT_TEST`, hardcoding no pin (board data,
ADR-0002).

**Pure model.** `BatteryModel` maps `(pinMilliVolts, dividerPermille)` to `BatteryLevel`. It scales to
cell millivolts (`pin * permille / 1000`), buckets by Li-ion thresholds (architecture-derived defaults,
tuning deferred), and applies a hysteresis band so the bucket moves only once a reading clears the
boundary by the band. `dividerPermille == 0` (uncalibrated) or an absent gauge returns `Unknown`. All
integer math (no float, no heap), so it fits the no-PSRAM budget and is asserted directly on the native
lane. `BoardProfile.batteryDividerPermille`: Stick `2000`, Cardputer/T-embed `0`, CYD `0`.

**Surface + view.** `ScreenToastSurface::setBatterySource(BatteryGauge&)` wires the gauge; on a throttled
tick (every few seconds) the surface reads the gauge, runs the model with `kActiveBoard`'s divider, and
writes `ScreenView.batteryShown` + `batteryLevel`. Only the bucket is in `ScreenView::operator==`, so
raw-millivolt dither never forces a re-render. On a bucket change the surface logs
`[BATT] mv=<cell> level=<name>`; a board with no gauge logs `[BATT] disabled (no gauge)` once at begin.
Wired in `hunt_loop.cpp` beside the hunt source, on a battery board only.

**Render.** `Esp32ScreenRenderer` draws a small battery icon (outline + fill segments from
`batteryLevel`, Critical in a warning colour) in the resolution-aware HUD layout; `Unknown`/`!batteryShown`
hides it.

## Verification

The pure model and the surface integration are host-testable; the ADC read and the icon render are not,
so this slice ships both a native lane and a device verify (§3).

- **`test/test_battery_model` (native, CI):** bucket boundaries (each threshold maps to the right
  `BatteryLevel`), hysteresis (a reading inside the band does not move the bucket; one clearing it does),
  and the fail-safe cases (`dividerPermille == 0` and an absent gauge both return `Unknown`, never a
  guessed level). Proves DoD scenario 3 and the uncalibrated/absent half of scenario 2.
- **`test/test_screen_toast_surface` (native, CI):** a fake `BatteryGauge` feeding the surface writes the
  expected `batteryLevel` into `ScreenView`, the throttle reads at the expected cadence, and a no-source /
  no-gauge surface leaves the battery hidden. Proves the view-model half.
- **Build check (native, CI + machine):** `cardputer`, `m5stickc_plus2`, `t_embed`, and `cyd_3248s035c`
  compile and link (the seam, model, profile field, view field, and renderer draw touch shared code).
- **`scripts/0092-battery-verify.mjs` (`verify:battery`, hardware lane):** on the attached Stick, asserts
  a `[BATT] mv=<cell> level=<name>` line appears with a plausible cell voltage (within a Li-ion range)
  and no `[FATAL]`, and writes an artifact for review. Wired in `package.json` (§4 #3 / R11). This is the
  machine-checkable half of DoD scenario 1.
- **Photographic (eyes-on, operator-run):** a HUD photo on the Stick showing the battery icon at a known
  charge (DoD scenario 1's icon half), per the boot-face/HUD photographic doctrine (§4 #22, ADR-0041). If
  not taken this session it is tracked in LEDGER like the earlier deferred HUD photos.

## As built

Shipped as designed and confirmed on device (M5StickC Plus2).

- The seam/model/device split landed as planned: `BatteryGauge` (`surface/battery_gauge.h`) forwards a raw
  `{present, pinMilliVolts}` reading; `Esp32BatteryGauge` reads a median of `analogReadMilliVolts(pin)`
  behind `#ifndef UNIT_TEST`; the pure `BatteryModel` (`surface/battery_model.*`) scales by the board
  divider, buckets by the Li-ion thresholds, and applies the hysteresis band. `BoardProfile` gained
  `batteryDividerPermille` (Stick `2000`, Cardputer/T-embed `0` = uncalibrated -> Unknown, CYD no pin).
  `ScreenView` gained `batteryShown` + `batteryLevel` (bucket only in `operator==`). The surface pulls the
  gauge on a throttle via `setBatterySource` and exposes getters; `Esp32ScreenRenderer` draws the icon;
  `hunt_loop` wires it on a calibrated board and emits `[BATT]`.
- `batteryCellMilliVolts` returns `uint32_t` end-to-end (not `uint16_t`): the adversarial correctness
  review (Sonnet, blind-to-intent) found the original narrowing would wrap a mis-authored divider (e.g.
  `20000` for `2000`) to a plausible-looking `464 mV -> CRITICAL`; widening makes a bad constant fail loud
  (an absurd `mv` the `[BATT]` line and `verify:battery` range check reject). Regression test
  `test_a_bad_divider_fails_loud_not_silently_truncated` added (fails pre-fix). The review's second finding
  (battery wiring coupled to display presence) was recorded as a deliberate HUD scope boundary (a code
  comment at the wiring site + a LEDGER feature line for headless serial telemetry), not expanded here.
- Two on-device layout refinements after eyes-on: the icon was moved from the status row up to the title
  row (in line with the product name), and the terminal nub was flipped to the left (fill anchored right)
  per the operator. These are cosmetic layout nudges (ADR-0002 §5), no ADR change.
- The `[BATT]` line emits on a bucket change **and** on a ~30 s heartbeat. The original edge-only line was
  unobservable to anything attaching after boot while the charge held steady (the first on-device
  `verify:battery` run timed out catching nothing); the heartbeat makes the current charge observable within
  a bounded window. ADR-0091 decision 6 was updated to match (pre-commit, still mutable).
- Machine checks (2026-10-28): lint 0/0, index regenerated, native suite 354/354 (was 340; +14 battery
  cases across `test_battery_model` and `test_screen_toast_surface`). All four display envs built SUCCESS
  with no `.dram0.bss` overflow (cardputer, m5stickc_plus2, t_embed, cyd_3248s035c).
- **DoD scenarios met on device (M5StickC Plus2, 2026-10-28):** flashed the normal build, provisioned via
  the portal, associated and hunting; `verify:battery` PASSED reporting `[BATT] mv=4238 level=FULL` (a
  plausible full Li-ion cell: pin ~2119 mV x the measured 1:2 divider), confirming the divider ratio reads
  a real voltage (scenario 1's machine half). The operator confirmed the panel shows the battery icon,
  nub-left and in line with the title, at a FULL fill matching the charge (scenario 1's icon half). The
  uncalibrated/absent-gauge -> Unknown path (scenario 2) and the hysteresis hold/release (scenario 3) are
  host-tested in `test_battery_model`/`test_screen_toast_surface`. `artifacts/0092-battery.txt` holds the
  `[BATT]` line (identifier-free, privacy guard clean). No deferred photo audit remains.
