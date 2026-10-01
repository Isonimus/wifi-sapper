---
id: '0069'
title: "RSSI-floor target ordering — widen the raw-bytes seam to carry discovery-time RSSI and partition the hunt"
type: slice
status: accepted
date: 2026-10-01
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0068: the hunt attempts stronger APs before weaker ones, so it spends its capture windows
where handshakes actually land. The clean field run (`field-2026-10-01`, 1330 attempts / 117 captured)
showed capture rate tracks signal — na 0% → `<-80` 2% → `-70..-80` 48% → `-60..-70` 76% — and that
restricting to `>= -80` dBm keeps 86% of captures (101/117) on 19% of windows (247/1330), ~4.6× density.
But the strongest band inverted (`>= -50` was 0/68, client-less airtime), so the lever is a **floor, not a
strongest-first sort**: partition the discovered set at -80 dBm, attempt the at-or-above partition first,
and attempt the below partition only when the former is exhausted — **deprioritise, never skip**, so the
endless hunt still captures every targetable AP (ADR-0068's "never a missed handshake" property). This
closes the `[feature] RSSI-floor target ordering` ledger item; the separate `[feature] never-captured-AP
backoff` item (the 0/68 story — a strong AP with no capturable handshake) stays open, distinct from this.

- **Seam (§4 #8):** `FrameConsumer::onFrame` widens to `onFrame(const uint8_t* frame, uint16_t len, int8_t
  rssi)`. The device callback already holds the datum (`pkt->rx_ctrl.rssi`); the seam forwards it as one
  out-of-band receiver datum alongside the raw bytes. No protocol interpretation is added — RSSI is
  receiver metadata, not frame content, so the pure-core/raw-bytes split that is #8's substance holds.
- **`ApRegistry`:** `DiscoveredAp` gains `int8_t rssi`, set from the forwarded value in the fully-written
  entry before the `count_` release-store (§4 #10). Discovery-time (first-sighting) RSSI, never mutated
  afterward — the registry's append-only / first-sighting-wins / lock-free discipline is preserved.
- **`HuntEngine`:** `doStartCapturing` applies a two-pass floor. Pass 1 walks discovery order attempting
  `rssi >= kRssiFloor`, skipping `< kRssiFloor` with the same `++targetIndex_; continue;` idiom the
  open/cracked/submitted skips use; at exhaustion it flips to pass 2 and re-walks attempting only the
  below-floor partition; pass-2 exhaustion re-discovers. The pass flag resets at the discovery snapshot.
- **Test double:** `FakeRadioSniffer::deliver` gains a defaulted strong `rssi`, so existing tests' beacons
  land on one side of the floor and their discovery-order assertions are unchanged.
- **§4:** edit #8 (the seam now also forwards `rx_ctrl.rssi`) and add invariant #31 (the floor partition),
  both in the same commit as this slice — the #28–#30 precedent (a row ships with its enforcing test).

## Definition of Done

**Scenario A — a strong AP is attempted before a weak one.**
- **Given** discovery enumerates AP W (weak, `rssi < -80`) earlier in discovery order and AP S (strong,
  `rssi >= -80`) later, both encrypted and capturable
- **When** the engine runs its discover→capture cycle
- **Then** the engine enters Capturing for S **before** W, inverting discovery order — the strong partition
  is attempted first despite W being discovered earlier.
- **Proof:** `test/test_hunt_engine` — fails before (W is attempted first, in discovery order), passes
  after. See `## As built`.

**Scenario B — a weak AP is deprioritised, never skipped (no missed handshake).**
- **Given** discovery enumerates only weak APs (`rssi < -80`), capturable
- **When** the engine exhausts the empty strong partition and reaches the weak partition
- **Then** the engine still captures a weak AP (raises `onCaptureReady` for it) — the below-floor partition
  is attempted when nothing stronger remains, so no targetable handshake is forfeited.
- **Proof:** `test/test_hunt_engine` — fails before (a floor that *skipped* `<-80` would capture nothing),
  passes after. See `## As built`.

**Scenario C — discovery order is preserved within each partition (no RSSI sort).**
- **Given** discovery enumerates two strong APs S1 then S2 (both `rssi >= -80`, S1 the weaker of the two)
  and a weak AP W, all capturable
- **When** the engine runs its capture cycle
- **Then** the attempt order is S1, S2, W — discovery order within the strong partition (S1 before S2
  though S2 is stronger: no RSSI sort), then the weak partition — so the strong-but-client-less anomaly is
  attempted in its discovery slot, never prioritised.
- **Proof:** `test/test_hunt_engine` — fails before (a strongest-first sort would order S2 before S1),
  passes after. See `## As built`.

**Scenario D — `ApRegistry` records the RSSI the seam forwards.**
- **Given** a beacon delivered through `ApRegistry::onFrame` with a known `rssi`
- **When** the entry is recorded
- **Then** the recorded `DiscoveredAp::rssi` equals the forwarded value, and the first sighting's value is
  kept when a later, differently-signalled beacon for the same BSSID arrives (first-sighting-wins).
- **Proof:** `test/test_ap_registry` — fails before (no `rssi` field / not recorded), passes after. See
  `## As built`.

## Design

ADR-0068 fixed the decision (floor at -80 dBm, deprioritise-never-skip, no sort); this slice fixes the
three implementation forks the ADR left open, each measured against the existing code.

**Fork 1 — how RSSI crosses the §4 #8 seam.** Searched `src/net/radio_sniffer.h` (the seam), its ESP32
impl, and every `onFrame` override/caller (`ApRegistry`, `HandshakeConsumer`, `HuntEngine`, `deauth_probe`,
the device callback, `FakeRadioSniffer`, `test_ap_registry`, `test_radio_sniffer`). Chosen: **widen the
signature** to `onFrame(frame, len, int8_t rssi)`. The datum is bound to its frame (no torn read), it is
explicit, and consumers that do not need it ignore the parameter. Rejected: a `FrameMeta{rssi}` struct
(speculative generality — only `rssi` is needed; global §3 / rule of three), and an out-of-band
`setLastRssi()` channel (splits one atomic datum across two calls — a race, and the "parallel path" §4 #2
forbids; it is what the *probe* does deliberately because the probe must not perturb the engine, whereas
production wants rssi bound to the frame).

**Fork 2 — which RSSI `ApRegistry` records.** Considered reusing the `isOpen`/`channel` pattern that
`DiscoveredAp` already uses (one value set once in the fully-written entry before the `count_`
release-store). Chosen: **first-sighting, immutable** — `rssi` joins that same write, never updated on later
sightings. The registry's whole concurrency contract (`ap_registry.h`: append-only, first-sighting-wins,
entry fully written then never mutated, lock-free single-producer/single-consumer) depends on entries not
being rewritten after publish; updating `rssi` per beacon would mutate a published entry on the driver task
while the app task reads it, a new data race needing per-entry atomics (§4 #10). Rejected:
strongest-across-sightings — it matches the probe's per-window-max measurement better, but the floor is
coarse (-80 is the 2%↔48% knee) and "deprioritise never skip" means a borderline AP whose first beacon
under-reads is merely attempted one pass later, never missed — so the precision is not worth a new
concurrency hazard. Recorded limitation: discovery-time RSSI is one beacon's reading, not the probe's
window max; revisit only if a field run shows the coarseness costs captures.

**Fork 3 — how selection partitions.** The current selection (`hunt_engine.cpp doStartCapturing`) is a
single `targetIndex_` walk in discovery order, skipping open/cracked/submitted with `++targetIndex_;
continue;`. Chosen: **two-pass flag** — `kRssiFloor = -80` and a `belowFloorPass_` bool. Pass 1 adds one
skip (`!belowFloorPass_ && rssi < kRssiFloor → continue`); on reaching `targetCount_` it flips the flag and
resets `targetIndex_ = 0` to re-walk the below-floor partition (`belowFloorPass_ && rssi >= kRssiFloor →
continue`); pass-2 exhaustion falls through to `doEnterDiscovering` as today. The flag resets to false at
the discovery snapshot (`tick` Discovering-deadline, alongside `targetIndex_ = 0`). This is +1 bool, +1
constant, and reuses the existing skip idiom. Rejected: precomputing an ordered index array at snapshot —
cleaner separation of static-ordering from live-skipping, but +64-byte member + a partition routine for
identical behaviour (KISS). The floor is read only on the app task in target selection, never on the
`onFrame` driver path, so §4 #8/#10 hold — exactly as the cracked/submitted/open filters do.

`kRssiFloor = -80` is the measured knee from `artifacts/0068-field-2026-10-01.txt`, not an estimate
(ADR-0068 decision 3). A below-floor sentinel default for `DiscoveredAp::rssi` keeps an accidentally-unset
entry in the deprioritised partition (fail toward later-but-never-missed), though every recorded entry gets
a real value from the seam.

## Verification

- **Floor partition + RSSI recording (Scenarios A–D)** — `pio test -e native` (`test_hunt_engine`,
  `test_ap_registry`), fail-before/pass-after recorded in `## As built`. No verify script: the behaviour is
  fully host-assertable (a fake sniffer feeding a real `ApRegistry` and `HuntEngine` through the real,
  now-widened `onFrame` seam), like the open/cracked/submitted filters — unlike an on-air proof. The
  measured basis for the floor value is the probe artifact `artifacts/0068-field-2026-10-01.txt` (ADR-0068);
  a probe answers its question once and the number lands in the ADR (§3), so no new probe here.

## As built

Shipped as designed, all three forks resolved as proposed. The `FrameConsumer::onFrame` seam widened to
`onFrame(const uint8_t* frame, uint16_t len, int8_t rssi)` (`src/net/radio_sniffer.h`); the device callback
forwards `pkt->rx_ctrl.rssi` (`radio_sniffer_esp32.cpp`), `HandshakeConsumer`/`deauth_probe` ignore it, and
every other caller (the engine forward, the HUD-stimulus injectors, the fake sniffer, the direct-call tests)
was migrated. `DiscoveredAp` gained `int8_t rssi = kRssiUnset` (`kRssiUnset = INT8_MIN`), set in the
fully-written entry before the `count_` release-store and never mutated after (`ap_registry.{h,cpp}`) —
first-sighting-wins by the existing early return. `HuntEngine` gained `kRssiFloor = -80` and a
`belowFloorPass_` flag; `doStartCapturing` became a two-pass walk (strong partition, then below-floor),
`advanceTarget` now always re-enters `doStartCapturing` so the pass flip is never bypassed, and the flag
resets at the discovery snapshot (`hunt_engine.{h,cpp}`). `FakeRadioSniffer::deliver` gained a defaulted
strong rssi, so pre-existing tests' discovery-order assertions are unchanged.

- **Scenario A — `test/test_hunt_engine::test_strong_ap_is_attempted_before_a_weaker_one`**: a weak AP
  discovered first and a strong one second are attempted strong-first, inverting discovery order.
- **Scenario B — `test/test_hunt_engine::test_weak_ap_is_still_captured_when_nothing_stronger_remains`**:
  the only AP being weak, pass 1 empties and pass 2 still captures it — deprioritise, never skip.
- **Scenario C — `test/test_hunt_engine::test_discovery_order_is_kept_within_a_partition_no_rssi_sort`**:
  two strong APs are attempted in discovery order (not strongest-first), then the weak one.
- **Scenario D — `test/test_ap_registry::test_records_rssi_from_the_seam_first_sighting_wins`**: the recorded
  `DiscoveredAp::rssi` equals the forwarded value, and a later differently-signalled beacon never overwrites
  it.
- **Fail-before/pass-after** (two coherent mutations, each reverted): (1) reproducing pre-slice behaviour —
  no partition skip and no pass flip, a single discovery-order pass — failed **only** Scenario A (`Expected
  170 Was 17`: the weak AP attempted first) while every other `test_hunt_engine` case, including round-robin
  and the deauth/snapshot cases, stayed green, proving the floor is what reorders; (2) a floor-as-*skip*
  (partition gate on, pass flip disabled so the weak tail is never attempted) failed Scenarios A, B, and C
  together, proving "deprioritise, never skip". Restored → full native suite **315/315**, `cardputer` and
  `cardputer_testhooks` firmware build, lint 0/0 (69 docs), INDEX regenerated and matching.

- **Adversarial pass (Sonnet 5, blind to intent / aware of law — the `src/`+`test/`+`CLAUDE.md` diff and the
  §4 table, told to read the files itself and not recurse, not shown the ADR/slice rationale):** one
  confirmed finding, fixed at root; the two-pass logic, the §4 #8/#10 concurrency discipline, the seam
  metadata, the tests, and the cost class were each independently hand-traced and cleared.
  - **Finding — the probe's engine would be biased by its own output.** `rssi_probe.cpp` forwarded the real
    `rx_ctrl.rssi` into the engine it runs; since the floor is always-on, a future probe rerun would measure
    a floored (reordered) engine, contradicting the probe's header/`onTapRx` comments that the RSSI never
    reaches the engine. **Fix:** the probe now hands the engine a fixed placeholder (`kProbeEngineRssi =
    -50`, above the floor so every AP shares one partition and the floor stays inert), keeping the plain
    discovery order ADR-0067's measurement depends on; the measured rssi still reaches the histogram tap
    only, and both comments were corrected. Confined to the `SAPPER_TEST_HOOKS` probe (exempt from the
    raw-bytes seam and from unit assertion); no production path affected, so no §4/LEDGER change.
