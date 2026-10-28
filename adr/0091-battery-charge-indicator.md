---
id: '0091'
title: "Battery charge indicator via a raw-millivolt HAL seam"
type: architecture
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Context

The appliance runs headless on battery in the field (the M5StickC Plus2's own profile note calls
this out: a true power-off needs USB unplugged because the unit is a battery appliance). An operator
holding a screen-equipped unit can see what it is hunting but not how much charge is left, so a field
unit can die mid-hunt with no warning. The one missing glance is remaining power.

The data to read it already exists but has never been read. ADR-0002 reserved `batteryAdcPin` in every
`BoardProfile` as board data for "a later slice" (Cardputer GPIO10, M5StickC Plus2 GPIO38, T-embed
GPIO4; the CYD declares -1, a USB-powered dev board with no battery gauge). This slice is that later
slice for the one board on the bench, the Stick.

Reading a usable charge level from that pin is nonlinear twice over, and both nonlinearities decide
where the interpretation belongs:

- The ESP32 SAR ADC is nonlinear and varies per chip. The Arduino core's `analogReadMilliVolts()`
  applies the factory eFuse calibration and returns calibrated millivolts *at the pin*. That is the
  raw receiver datum this peripheral produces, the battery analogue of the `wifi_pkt_rx_ctrl_t.rssi`
  the RX seam already forwards under §4 #8: a calibrated hardware reading, not an interpreted value.
- The board scales the cell down to the ADC's input range with a resistor divider. The Stick's is a
  measured 1:2 (its profile note records GPIO38 as the real divider tap, reused from the on-device
  `esp32-dragon-ball-radar` HAL), so cell millivolts = pin millivolts times the divider ratio. The
  ratio is a per-board fact, the same kind of datum ADR-0002 keeps in the profile.

State of charge from a single voltage reading is only ever coarse. The Li-ion discharge curve is
nearly flat across most of its usable range (roughly 3.9 V down to 3.6 V), so a voltage reading maps
to a wide span of remaining capacity. A percentage implies a precision this hardware cannot deliver
without coulomb counting, which it does not have. The honest surface is a coarse bucket, matching the
status LED's coarse-status philosophy (ADR-0021) and the measure-don't-estimate bar: show what the one
reading can actually support, a few charge levels, not a fabricated "73%".

## Decision

1. **A `BatteryGauge` HAL seam forwards raw calibrated millivolts only.** The abstract seam returns a
   small result: whether a gauge is present and, if so, the calibrated pin millivolts. The device
   implementation `Esp32BatteryGauge` reads `analogReadMilliVolts(profile.batteryAdcPin)` (a median of
   a few samples to reject ADC noise) behind `#ifndef UNIT_TEST`, and reports "no gauge" when
   `batteryAdcPin < 0`, exactly as a screenless board resolves to a `NullDisplay`. The seam carries no
   divider ratio and no charge interpretation: this is §4 #8's raw-datum discipline applied to a new
   peripheral, so the whole interpretation is pure and host-tested, and porting to a new board is a new
   gauge plus board data, never new policy (the LED seam's split, ADR-0021).

2. **A pure `BatteryModel` maps (pin millivolts, divider ratio) to a coarse `BatteryLevel`, with
   hysteresis.** It scales pin millivolts by the board's divider to cell millivolts, then buckets the
   result into `BatteryLevel` (Unknown, Critical, Low, Mid, High, Full: a four-segment icon plus an
   Unknown state). The thresholds are architecture-derived Li-ion defaults, tuned later from a real
   discharge run (deferred, LEDGER), like the dwell/settle/deauth-cadence/backoff constants. Hysteresis
   (a small millivolt band around each boundary, where the bucket only moves once the reading clears the
   band) is why this logic is a pure model and not an inline comparison: it stops the icon flickering
   between two levels when the voltage dithers at a threshold, and that debounce is asserted on the
   native lane against scripted readings. An uncalibrated board (divider ratio 0) or an absent gauge
   reports `Unknown`, never a guessed level: an un-measured divider is not an estimate we ship.

3. **`BoardProfile` gains `batteryDividerPermille`, board data under ADR-0002.** The Stick is `2000`
   (the measured 1:2, dragon-ball), confirmed on-device in this slice (the `[BATT]` line against a
   multimeter). The Cardputer and T-embed are left `0` (uncalibrated, so the model reports Unknown)
   until each divider is measured on its own board, a one-line data change then, tracked in LEDGER. The
   CYD keeps `batteryAdcPin = -1` (no gauge) so its ratio is moot.

4. **The gauge is injected into the HUD surface like the hunt snapshot (ADR-0033).** The pure
   `ScreenToastSurface` gains `setBatterySource(BatteryGauge&)` and, on a throttled tick (charge moves
   over minutes, not loop iterations), reads the gauge, runs the `BatteryModel` with the active board's
   divider, and writes the resulting bucket into `ScreenView`. `ScreenView` gains `batteryShown` and
   `batteryLevel`; only the bucket is part of `operator==`, so raw-millivolt dither never forces a
   re-render (the view changes only when the level changes). The injected-source shape means a host test
   drives a fake gauge, so the throttle and the surface integration are asserted on the native lane like
   the hunt source.

5. **One renderer draws the icon this slice.** `Esp32ScreenRenderer` (the sprite renderer the
   Cardputer, Stick, and T-embed share) draws a small battery icon with fill segments from
   `batteryLevel`, in the resolution-aware layout slice-0088 already established (no new deferral there).
   The `DirectPanelScreenRenderer` deliberately ignores the field: its only board, the CYD, has no gauge
   (`batteryAdcPin -1`), so drawing a battery icon there would be speculative code no hardware exercises
   (§3, KISS). A future direct-panel battery board adds that draw then, on a present need, tracked in
   LEDGER. This is consistent with §4 #39's two-renderer split: a direct-panel capability is added when a
   direct-panel board needs it, not pre-emptively.

6. **A serial `[BATT]` line is the machine-checkable verify half.** The device loop logs
   `[BATT] mv=<cell> level=<name>` on each bucket change and on a slow heartbeat (~30 s). The heartbeat is
   deliberate, not spam: unlike the LED (whose verify probe re-drives its states), the battery bucket is
   stable, so an edge-only line would be invisible to anything attaching after boot (the verify, or an
   operator opening the serial monitor) while the charge holds steady; the heartbeat guarantees the current
   charge is observable within a bounded window. A board with no gauge logs `[BATT] disabled (no gauge)`
   once. The physical icon is the human half of the verify, the serial line the half a script can assert on
   the hardware lane.

This **extends** ADR-0002 (a reserved board-data pin finally read, plus a new board-data field), ADR-0021
(the HAL-seam/pure-policy split the LED established, applied to a second peripheral), and ADR-0033 (the
injected pull-source shape for a HUD input). It **supersedes none**: no earlier decision claimed the
battery pin would be read a particular way, and the coarse-bucket choice is the first decision on charge
display, not a reversal.

## Consequences

- **A new §4 standing invariant (battery seam purity).** Battery voltage interpretation (divider
  scaling, bucketing, hysteresis) lives in the pure core; the `BatteryGauge` seam forwards raw
  calibrated millivolts only. This is the battery analogue of #8 (RX raw bytes/rssi) and #15 (TX raw
  frames): a new peripheral seam that must stay dumb so its policy stays host-tested. The row is added in
  this commit.
- `BoardProfile` gains `batteryDividerPermille` (data, ADR-0002), Stick `2000`, others `0` until
  measured. `ScreenView` gains `batteryShown` + `batteryLevel` (POD, host-asserted; only the bucket is
  compared).
- `README.md`'s surface/feature description is updated in the same change (live doc): a screen-equipped
  board shows a battery icon on the HUD, on a board whose divider has been measured.
- Verification: the pure `BatteryModel` (bucket boundaries, hysteresis, uncalibrated/absent -> Unknown)
  and the surface integration with a fake gauge are native tests that run in CI; the on-device half is
  `scripts/0092-battery-verify.mjs` (`verify:battery`), which asserts the `[BATT] mv=` line reports a
  plausible cell voltage with no `[FATAL]` on the attached Stick and writes an artifact for review; the
  HUD icon itself is photographic (eyes-on), like the splash and the live HUD.
- Deferrals recorded in `LEDGER.md`: measure the Cardputer and T-embed dividers (one data change each);
  battery on the Maintenance screen; charging-state detection (the Stick's PMIC/charge status is an I2C
  read, out of this slice); threshold tuning from a real discharge curve; a direct-panel battery icon
  when a direct-panel battery board exists.
