---
id: '0068'
title: "Widen the raw-bytes seam for an RSSI-floor target order — the field run showed capture rate tracks signal"
type: architecture
status: accepted
date: 2026-10-01
supersedes: []
superseded_by: []
---

## Context

ADR-0067 decision 3 refused to widen the §4 #8 raw-bytes seam for RSSI-ordering on an estimate, and
gated it on a field-measured yield gain. Its fourth amendment voided the first field run (the RSSI tap
was a defective instrument — table-overflow blindness and adjacent-channel bleed) and required a fresh
one off the rebuilt per-attempt on-channel-max tap. This ADR is the follow-up that decision 3 promised:
it reads the clean run and makes the seam decision.

The run — tag `field-2026-10-01`, **1330 resolved attempts / 117 captured** over ~3 h on authorized APs,
the identity-free per-band histogram (§4 #25), binned by the strongest on-channel beacon RSSI heard while
the engine was parked capturing each target (ADR-0067 amendment 4):

```
band                    captured / total   rate
>= -50 (very strong)         0 / 68       0.0%
-50..-60 (strong)            0 / 1        0.0%
-60..-70 (moderate)         42 / 55       76.4%
-70..-80 (weak)             59 / 123      48.0%
< -80 (very weak)           16 / 808       2.0%
rssi=na (unknown)            0 / 275       0.0%
```

**The necessary condition ADR-0067 set is met: capture rate tracks signal strength, and the effect is
large.** Read from the bottom up it climbs cleanly and monotonically — na 0% → `<-80` 2% → `-70..-80` 48%
→ `-60..-70` 76%. The low tail (`<-80` plus `na`) holds **1083 of 1330 attempts (81%) at a ~1.5% capture
rate**, while the moderate band captures at 76% — a ~38× yield difference across the bands that hold the
attempts.

**But the two strongest bands invert, and that shapes the decision.** `>= -50` is **0/68**: zero captures
over 68 attempts at the strongest signal. (`-50..-60` is 0/1 — a single attempt, noise.) The 0/68 is not
an instrument artifact — capture is measured independently of the RSSI tap (an on-air 4-way handshake,
not a tap reading). Its most likely cause, diagnosed rather than assumed: one or a few very-close
**client-less (or PMF-protected) APs** — plausibly the operator's own fixed infrastructure — re-targeted
every enumeration cycle. No associated client means no 4-way handshake regardless of signal; and because
such an AP is never captured, it is never submitted or cracked, so the #28/#30 gates never skip it, so it
is re-attempted each cycle and piles up dead windows in the top band. A recorded limitation: the
histogram is identity-free (§4 #25), so this run's data **cannot prove** the 68 are one AP rather than
many — the single-AP reading is the likeliest explanation, not a measured fact.

**This refutes the naive "strongest-first" lever ADR-0067 reached for.** ADR-0067 borrowed the parent
Adversary's "attack strongest-first" scorer as the hypothesis. The data contradicts it at the top: the
strongest band is dead weight, so ordering strictly by descending RSSI would attack client-less airtime
*first*. The gain is not in preferring the strongest — it is in **not spending airtime on the weak / no-
signal tail**. Quantified: restricting attempts to `>= -80` dBm keeps **101 of 117 captures (86%)** on
**247 of 1330 windows (19%)** — capture density rises from 8.8% to 40.9%, ~4.6×, for the cost of forgoing
16 handshakes in the `<-80` band.

## Decision

1. **Widen §4 #8 to carry the receive RSSI across the seam.** The `FrameConsumer`/`RadioSniffer` seam
   forwards the beacon's `wifi_pkt_rx_ctrl_t.rssi` alongside the raw frame bytes, and `ApRegistry` records
   a per-AP discovery-time RSSI. This is the architectural cost ADR-0067 declined to pay on an estimate;
   the field run is the measured justification. The seam still carries **no protocol interpretation** —
   RSSI is receiver metadata, not frame content, so the pure-core/raw-bytes split that is #8's substance
   holds; what widens is that the seam now also forwards one out-of-band receiver datum.

2. **Target selection applies an RSSI *floor*, not a sort.** `HuntEngine` partitions the discovered set
   into at-or-above vs below the floor, attempts the at-or-above partition first, and attempts the below
   partition only when the former is exhausted. Discovery order is preserved *within* each partition — no
   RSSI sort.

3. **The floor is -80 dBm** — the measured knee between the 48%-yield `-70..-80` band and the 2%-yield
   `<-80` band.

4. **Deprioritise, never skip.** The below-floor partition is still attempted when nothing stronger
   remains, so the endless hunt still captures every targetable AP: ADR-0067's "never a missed handshake,
   only un-optimised ordering" property is preserved. A floor that *skipped* `<-80` would forfeit the 16
   captures that band still produced.

5. **No RSSI sort within the above-floor partition.** The 0/68 at `>= -50` shows strongest-first would
   prioritise dead, client-less airtime. A binary floor treats every `>= -80` target equally (discovery
   order), so the strong-but-client-less anomaly is neither prioritised nor specially handled — it is
   attempted in its discovery slot like any other above-floor AP.

6. **The implementation is a slice, not this ADR.** This ADR records the decision and its measured basis
   (the follow-up ADR-0067 decision 3 promised). The seam widening, the engine partition, their
   regression tests (`test/test_ap_registry` for the recorded RSSI, `test/test_hunt_engine` for the floor
   partition), and the new §4 row ship together in that slice — which runs the blind adversarial pass
   before merge, because it touches §4 #8 and target selection. A `LEDGER.md` line carries the deferral.

## Consequences

- §4 #8 is widened by this decision but not yet in code. The #8 row is edited and a new target-selection
  invariant row (#31) added **in the implementing slice's commit, not here** — the #28–#30 precedent: an
  invariant row ships with the test that enforces it, never ahead of it.
- The `na` bucket (0/275) is a **probe artifact, not a production state**. The probe tapped per-window
  on-channel RSSI and recorded `na` when the target's beacon was not re-heard inside its capture window;
  production selection will use the discovery-time RSSI that every `ApRegistry` entry now carries, so there
  is no "unknown-RSSI" target to order. The probe's `na` only reinforces the gradient's floor — no signal
  in-window, no capture.
- A separate, **non-RSSI** waste is surfaced and deferred (LEDGER): the strong-but-never-captured AP (the
  0/68 story) is re-targeted every enumeration cycle because #28/#30 skip only *cracked/submitted* BSSIDs
  and an AP with no capturable handshake is neither. A bounded "attempted N times, never captured → back
  off" lever would reclaim that airtime; it is distinct from the RSSI floor (that AP is *strong*) and out
  of scope here.
- Limitation recorded: this is one ~3 h window on one site's AP/client population, and -80 is that run's
  measured knee. The implementing slice may add a short dedicated tuning pass, but -80 is the data-grounded
  default, not an estimate.
- ADR-0067 decisions 1 (no channel grouping), 2 (no WPA2-preference), and 4 (no feature/slice yet) stand.
  This ADR resolves only its decision 3 — the deferred seam decision — exactly as that decision said a
  follow-up ADR would.
