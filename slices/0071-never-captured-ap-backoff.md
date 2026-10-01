---
id: '0071'
title: "Never-captured-AP backoff"
type: slice
status: accepted
date: 2026-10-01
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0070: stop the endless hunt from spending a capture window every cycle on a strong but
client-less (or PMF-protected) AP that never yields a handshake. This closes the LEDGER feature
"capture: never-captured-AP backoff" (the `field-2026-10-01` run's `>= -50` band, 0/68, plus `na` 0/275 —
343 windows, 26% of the run, that neither the cracked/submitted/open filters nor the RSSI floor reclaim).
`HuntEngine` gains a per-BSSID record of consecutive empty capture windows; an AP that yields nothing for
`kBackoffMissThreshold` windows is skipped in target selection for `kBackoffCadenceCycles` enumeration
cycles, then re-attempted once — a deliberate, evidence-gated deviation from #31's "deprioritise, never
skip" (ADR-0070 decision 3). A capture clears the record and rehabilitates the AP. It leaves open the
distinct rotated-PSK wall-clock re-verification lever (a separate LEDGER item) and the constant-tuning
deferral this slice records.

## Definition of Done

- **Scenario A — a persistently empty AP is backed off after the threshold**
  - **Given** an armed endless hunt and one AP that beacons every cycle but never completes a handshake
  - **When** it has been attempted `kBackoffMissThreshold` times with no capture
  - **Then** it is no longer selected as a target on the immediately following cycles (its capture windows
    are reclaimed), while a second AP that *does* capture is still attempted every cycle

- **Scenario B — a backed-off AP is re-attempted after the cadence (never permanently skipped)**
  - **Given** an AP that has been backed off after `kBackoffMissThreshold` empty windows
  - **When** `kBackoffCadenceCycles` enumeration cycles have elapsed
  - **Then** it is attempted again (so a client that has since associated is still caught), and if that
    retry is also empty it is suppressed again — one retry window per cadence, not a renewed N-window run

- **Scenario C — a capture clears the backoff state**
  - **Given** an AP that has accrued empty windows (at or below the threshold)
  - **When** it later completes a wpa-sec-valid handshake
  - **Then** its consecutive-miss count is reset, so it is treated as a fully productive AP from then on

- **Scenario D — backoff is gated on misses, so a productive hunt is unchanged; all-backed-off re-discovers**
  - **Given** APs that capture within the threshold (the normal case)
  - **When** the hunt runs
  - **Then** no AP is ever backed off (the pre-ADR target sequence holds); and **given** every discovered
    AP is in backoff, **then** the selection loop exhausts to a fresh discovery sweep, never a spin

## Design

Grounded in the current `HuntEngine` (hunt_engine.cpp), which ADR-0068 just extended with the two-pass
RSSI floor. What was considered, reused, and deliberately not reused:

- **Outcome signal — reused, not added.** `tick()`'s Quiescing resume already distinguishes
  `ReportThenAdvance` (captured) from `AdvanceNoReport` (window expired empty); the current target's BSSID
  is `collector_.handshake().bssid`, read on the app task with the router aimed away and settled (the same
  safe read the capture-ready observer uses). Backoff hooks these two resumes — no new capture-success
  plumbing.
- **State location — in-engine, not the registry, not a lookup seam.** The `ApRegistry` is `reset()` every
  discovery cycle and is driver-task-written under a single-producer discipline, so it cannot hold
  app-task-mutated cross-cycle counters (the same reason RSSI is a first-sighting field, not a counter).
  The cracked/submitted `*Lookup` seams are *read-only external* state written elsewhere; backoff is the
  engine's own record of its own attempts, so it is intrinsic engine state (like `targetIndex_` /
  `belowFloorPass_`), not a seam. A bounded per-BSSID table + a cycle counter live in `HuntEngine`.
- **Skip vs reorder — skip, with justification.** Folding backoff into the floor's below-floor partition
  (a pure reorder) was rejected: a cycle drains every partition, so a reorder does not reclaim the windows
  (ADR-0070 Context). Backoff genuinely skips for a cadence — a measured-evidence deviation from #31,
  recorded as ADR-0070 decision 3.
- **Cadence unit — cycles, not wall-clock** (ADR-0070 decision 4): the reclaimed cost is one window per
  cycle, so cycles are the natural unit and keep the mechanism host-testable with no fake clock.
- **Constants** (ADR-0070 decision 7): `kBackoffMissThreshold = 3`, `kBackoffCadenceCycles = 5`,
  `kMaxBackoffEntries = kMaxDiscoveredAps`, public in hunt_engine.h beside `kRssiFloor` so the tests name
  them; architecture-derived defaults with a tuning LEDGER item, like dwell/settle/deauth-cadence.

Mechanism: a `cycleCounter_` increments at each discovery snapshot. A new skip gate in `doStartCapturing`
(alongside the open/cracked/submitted/floor filters) skips a target whose BSSID is in backoff
(`backoffUntilCycle > cycleCounter_`). On `AdvanceNoReport` the target's miss count increments (inserting
it, evicting the least-penalised slot if the table is full) and, at the threshold, its
`backoffUntilCycle` is set to `cycleCounter_ + kBackoffCadenceCycles`. On `ReportThenAdvance` the entry is
cleared. All writes/reads are app-task only; `onFrame` is untouched (§4 #8/#10 hold).

## Verification

Host-assertable in full (the backoff logic is pure state-machine behaviour over the injected fake sniffer
and fake clock — no rendering/timing/RF), so there is no verify script (ADR-0004: a slice ships one only
when a unit test cannot assert the behaviour). New regressions in `test/test_hunt_engine`:

- Scenario A — `test_never_capturing_ap_is_backed_off_after_the_threshold`: a never-capturing AP drops out
  of the attempt sequence after `kBackoffMissThreshold` cycles while a productive AP keeps being attempted.
- Scenario B — `test_backed_off_ap_is_reattempted_after_the_cadence`: after `kBackoffCadenceCycles` cycles
  the backed-off AP is attempted again, then re-suppressed.
- Scenario C — `test_capture_clears_the_backoff_miss_count`: an AP that captures after accruing misses is
  never backed off thereafter.
- Scenario D — `test_productive_hunt_never_backs_off` (gated-on-misses) and
  `test_all_backed_off_degrades_to_rediscovery_not_a_spin` (all-filtered fall-through).

Fail-before is shown by neutralising the skip gate (the AP keeps being attempted every cycle → Scenario A
fails) and the cadence re-enable (a permanently-skipped AP → Scenario B fails); recorded in `## As built`.

## As built

Shipped as designed. `HuntEngine` gained a bounded per-BSSID backoff table (`BackoffEntry[kMaxBackoffEntries]`,
packed, linear-scanned), a `cycleCounter_`, and three public constants beside `kRssiFloor`
(`kBackoffMissThreshold = 3`, `kBackoffCadenceCycles = 5`, `kMaxBackoffEntries = kMaxDiscoveredAps`). The
counter increments at the discovery snapshot (`tick()`/Discovering); a new skip gate in `doStartCapturing`
(after the open/cracked/submitted filters, inside the two-pass floor walk) skips an AP while
`backoffUntilCycle > cycleCounter_`; the Quiescing-resume handler calls `registerCaptureOutcome` —
`AdvanceNoReport` increments the miss count (inserting, evicting the least-penalised slot if full) and sets
the horizon at the threshold, `ReportThenAdvance` clears it. `consecutiveMisses` saturates at `UINT16_MAX`;
all state is app-task only, `onFrame` untouched (§4 #8/#10).

Verification — all four Definition-of-Done scenarios met, as five `test/test_hunt_engine` regressions
(320/320 native, was 315; cardputer + cardputer_testhooks both build):

- Scenario A → `test_never_capturing_ap_is_backed_off_after_the_threshold`
- Scenario B → `test_backed_off_ap_is_reattempted_after_the_cadence` (incl. one-window-per-cadence re-suppress)
- Scenario C → `test_capture_clears_the_backoff_miss_count`
- Scenario D → `test_productive_hunt_is_never_backed_off` + `test_all_backed_off_degrades_to_rediscovery_not_a_spin`

Fail-before shown with two coherent, isolated mutations: disabling the skip gate
(`if (false && isBackedOff(...))`) failed A, B, and D's all-backed-off case while leaving C and the
productive-hunt case green; disabling the capture-clear failed C alone. Both reverted.

Adversarial pass (blind to intent, aware of law; Sonnet 5; read the files itself, no recursion) — required
because this adds §4 invariant #32 and changes target selection. It attacked the cycle-counter off-by-one,
the table eviction, the floor-walk interaction (spin / skip-forever / wrong-AP), integer wrap, the §4
#8/#10 concurrency claim (grepping every read/write site), BSSID attribution at the resume sites, and
miss-counting of skipped targets. **No findings** — every scenario was verified handled. No change resulted.
