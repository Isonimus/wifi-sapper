---
id: '0070'
title: "Back off a never-captured AP for a cadence — the RSSI floor cannot reclaim strong client-less airtime"
type: architecture
status: accepted
date: 2026-10-01
supersedes: []
superseded_by: []
---

## Context

ADR-0068's Consequences surfaced a waste the RSSI floor it decided (#31) **cannot** reclaim, and deferred
it to a lever of its own (LEDGER). This ADR is that lever.

The `field-2026-10-01` run (1330 attempts / 117 captured) binned capture rate by signal:

```
band                    captured / total   rate
>= -50 (very strong)         0 / 68       0.0%
-50..-60 (strong)            0 / 1        0.0%
-60..-70 (moderate)         42 / 55       76.4%
-70..-80 (weak)             59 / 123      48.0%
< -80 (very weak)           16 / 808       2.0%
rssi=na (unknown)            0 / 275       0.0%
```

The `>= -50` band is **0/68** — zero captures over 68 windows at the *strongest* signal. ADR-0068
diagnosed the likeliest cause (not a measured fact, since the histogram is identity-free, §4 #25): one or
a few very-close **client-less or PMF-protected APs** — plausibly the operator's own fixed infrastructure
— re-targeted every enumeration cycle. No associated client means no 4-way handshake regardless of signal.

**None of the existing target-selection filters reclaim this.** #28 (cracked) and #30 (submitted) skip
only a BSSID that was captured-then-landed-at-wpa-sec; an AP with no capturable handshake is never
captured, so it is never submitted or cracked, so those gates never fire. #29 (open) skips only an
unencrypted BSS; a client-less WPA AP still advertises Privacy. And **#31 — the RSSI floor just shipped —
makes it worse, not better, for this AP**: the floor *deprioritises the weak tail*, but this AP is
`>= -50`, so it sits in the **strong partition the floor attempts first**. The floor spends the top of
every cycle on exactly the airtime that captures nothing.

Reading the code confirms a pure reorder would not help either. A hunt cycle drains *every* partition:
`doStartCapturing`/`advanceTarget` walk the whole enumeration to `targetCount_`, giving each targetable AP
one `captureWindowMs`, before re-discovering — there is no time-bound that cuts a cycle short. So merely
sorting a client-less AP later in the cycle still spends its window every cycle; the 68 dead windows move,
they do not vanish. To *reclaim* the airtime (ADR-0068's stated goal) the AP must be genuinely **not
attempted** for a span of cycles — a skip, not a reorder.

## Decision

1. **Back off an AP that yields no handshake in `kBackoffMissThreshold` consecutive capture windows.**
   `HuntEngine` keeps a per-BSSID count of consecutive empty capture windows. The count increments when a
   Capturing phase ends with no wpa-sec-valid handshake (`Resume::AdvanceNoReport`) and is cleared the
   moment that BSSID is captured (`Resume::ReportThenAdvance`) — gate on demonstrated unproductivity, the
   same knowledge-not-hope discipline as #27/#30. At the threshold the AP enters backoff.

2. **A backed-off AP is skipped in target selection for a bounded cadence, then re-attempted — never
   permanently skipped.** The engine counts enumeration cycles (one per discovery snapshot); backoff
   suppresses the AP until the counter advances by `kBackoffCadenceCycles`, then it is attempted again. If
   that single retry window is also empty the count (never reset except by a capture) is already at/over
   threshold, so it re-enters backoff immediately — **one** retry window per cadence, not a fresh N-window
   investment each time. A capture at any point clears the state and fully rehabilitates the AP.

3. **This is a deliberate, evidence-gated deviation from #31's "deprioritise, never skip."** #31 never
   *skips* a weak AP because weak signal does not imply no handshake — skipping forfeits a real capture.
   Backoff *does* skip, but only on **measured unproductivity** (N consecutive empty windows, with armed
   deauth forcing reassociation throughout), which is direct evidence the AP has no capturable handshake
   *right now*; and the cadence retry means a client that later associates is still caught within a bounded
   delay. Different evidence, different rule — recorded here as the §2 "justified violation is a decision,
   not a silent exception".

4. **The cadence is counted in enumeration cycles, not wall-clock.** The cost being reclaimed is "one
   capture window per cycle," so skipping K *cycles* reclaims exactly K windows; cycles need no clock and
   keep the whole mechanism host-assertable without a fake timer. (Wall-clock is what the rotated-PSK
   re-verification lever needs — a real-elapsed-time event — and is a separate, still-open LEDGER item.)

5. **Always on, intrinsic engine state — no external lookup, never on the frame path.** Like #29 (open)
   and #31 (floor), backoff needs no provisioned or persisted state: it is the engine's own record of what
   its own capture attempts yielded. The table, the miss counts, and the cycle counter are written only on
   the app task (in the Quiescing-resume handler and at the discovery snapshot) and read only on the app
   task (target selection) — `onFrame` never touches them, so §4 #8/#10 hold exactly as for gates B/C.

6. **The backoff table is bounded and fail-toward-attempting.** It holds at most `kMaxBackoffEntries` APs
   (one sweep's worth); when full, inserting a newly-missed AP evicts the least-penalised entry (smallest
   suppression horizon — a capture-cleared or already-expired slot goes first). A mis-eviction only drops
   an AP's backoff, so it is re-attempted — degrading to the pre-ADR behaviour for that one AP, never a
   crash and never a missed capture. All-backed-off degrades to re-discovery, like #28/#29/#30's
   all-filtered fall-through — never a spin.

7. **The three constants are architecture-derived defaults with a tuning deferral (LEDGER).** `kBackoff
   MissThreshold = 3`, `kBackoffCadenceCycles = 5`, `kMaxBackoffEntries = kMaxDiscoveredAps`. The threshold
   gives an AP three full capture+deauth windows before writing it off; the cadence reclaims ~4 of every 5
   cycles for a confirmed-dead AP while re-probing often enough to catch a newly-arrived client. These are
   grounded in the one `field-2026-10-01` run, which proves the *waste is material* (the `>= -50` 0/68 plus
   `na` 0/275 are 343 windows, 26% of the run) but is one ~3 h site — not enough to call the exact numbers
   measured. A second field run tunes them, exactly as dwell/settle/deauth-cadence are each defaulted +
   LEDGERed. They are constants, not `HuntConfig` fields: a future tuning pass supersedes the values, it
   does not make them per-instance (KISS, as #31's `kRssiFloor`).

## Consequences

- A new §4 invariant row (#32) ships in this ADR's implementing slice's commit with the `test_hunt_engine`
  regressions that enforce it — the #28–#31 precedent, an invariant row never ahead of its test.
- The `>= -50` 0/68 and `na` 0/275 waste is reclaimed after the initial threshold investment: a confirmed
  client-less AP costs 3 windows once, then ~1 window per `kBackoffCadenceCycles` cycles, instead of one
  every cycle forever.
- Composes with #31 without disturbing it: the backoff skip is a filter *within* the two-pass floor walk
  (checked in both passes), so a backed-off AP is skipped in whichever partition it belongs to; the
  partition machine and the `advanceTarget` re-entry it depends on are unchanged.
- Limitation recorded (LEDGER): the constants are one-run defaults; and the cycle counter is a plain
  `uint32_t` not wrap-guarded like the millis deadlines, because at roughly one cycle per minute it would
  take millennia to wrap — unreachable, unlike the ~49-day millis wrap the settle guards against.
- The rotated-PSK re-verification lever (a *wall-clock* re-enable of cracked/submitted BSSIDs) stays open
  and distinct: that re-probes an AP we *did* capture; this re-probes one we never could. Neither subsumes
  the other.
