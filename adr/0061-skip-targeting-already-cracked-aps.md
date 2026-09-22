---
id: '0061'
title: "Skip targeting already-recovered APs in the hunt (gate B of ADR-0059)"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

ADR-0059 gate A stopped the appliance *re-uploading* handshakes for networks it has already recovered,
by consulting the read-only `CrackedLookup` seam in the `UploadSupervisor` drain. But the waste starts one
step earlier: the endless hunt still *targets* an already-cracked AP on every discover→capture cycle —
sitting on it for a full `captureWindowMs`, and, when armed (ADR-0029), broadcasting deauth/disassoc at it
— to collect a handshake gate A will then throw away. That is airtime and, worse, on-air disruption spent
on a network we are done with.

ADR-0059 decision #5 deferred this to its own ADR precisely because it is a **boundary change**:
`HuntEngine` is otherwise *purely frame-fed*. Its correctness inputs are the radio frames (interpreted in
the pure capture core, §4 #8) and the clock; the RX callback does nothing but forward bytes (§4 #10). It
holds no reference to persisted device state. Gate B gives the engine its first read-only persisted-state
input — the same `CrackedLookup` gate A introduced — so it deserves a recorded decision about *how* that
input enters without eroding the frame-fed purity those invariants protect.

The mechanics are favourable. `HuntEngine::doStartCapturing` already walks the discovered targets in a
`while (targetIndex_ < targetCount_)` loop and *skips* any target whose channel the radio rejects
(`++targetIndex_; continue;`), falling through to `doEnterDiscovering()` when none remain. Both capture
entry points (`doStartCapturing` from the post-discovery quiesce, and `advanceTarget` after each target)
route through that one loop. And that loop runs on the **app task** inside `tick()`/the quiesce resume —
never in the `onFrame` driver callback — so a lookup consulted there touches neither §4 #8 nor #10.

## Decision

1. **Reuse the `CrackedLookup` seam, injected by an off-by-default setter.** `HuntEngine` gains
   `void setCrackedLookup(const CrackedLookup&)` (mirroring ADR-0059's `UploadSupervisor::setCrackedLookup`
   and the engine's own optional `RawTransmitter`), storing a `const CrackedLookup* crackedLookup_ =
   nullptr`. **Unset (`nullptr`) → the engine is exactly the purely-frame-fed engine it is today**: it
   targets every discovered AP, and §4 #8/#10 hold unchanged. The persisted-state input is therefore
   opt-in and read-only — the engine never mutates it and never depends on it to interpret a frame; it is a
   *target filter*, not loop state.

2. **Skip a cracked target in the one selection loop.** In `doStartCapturing`, before `retuneTo`, if
   `crackedLookup_ && crackedLookup_->isCracked(registry_.at(targetIndex_).bssid)`, treat it exactly like a
   channel-rejected target — `++targetIndex_; continue;` — so it is never tuned to, never captured, and
   (being never the `capturingBssid()`) never deauthed (§4 #16). Because `advanceTarget` also funnels
   through this loop, one insertion covers the whole round-robin.

3. **All-cracked degrades to re-discovery, never a spin.** If every discovered AP is already cracked, the
   loop exhausts and calls `doEnterDiscovering()` — a fresh sweep bounded by `discoverWindowMs`, exactly
   the existing "no capturable target remained" path. The appliance keeps hunting for *new/uncracked* APs
   at the normal discovery cadence; it simply captures nothing until one appears. No busy-loop, no stuck
   phase.

4. **Same task as the manifest writer — no lock.** The lookup is read on the app task (target selection);
   `g_manifest`, its concrete implementer, is written on the same cooperative main-loop task (via
   `CrackedSync` inside the `UploadSupervisor` drain cycle). There is no separate RTOS task for sync, and
   the driver-task RX callback never touches the manifest. So the read is race-free without a lock, like
   gate A's.

5. **Gate on knowledge, not hope (inherited).** Only a BSSID actually present in the recovered manifest is
   skipped, so an uncracked network is always still targeted and captured — that capture is how it gets
   cracked (identical to ADR-0059 decision #4).

## Consequences

- The armed appliance stops spending capture windows and deauth bursts on networks it has already cracked,
  freeing that airtime for uncracked targets and sparing a done-with network the on-air disruption. This is
  the targeting-side complement of gate A; together they close the redundant-work loop ADR-0059 opened.
- **New §4 invariant #28**, added in this commit: the engine skips an already-cracked BSSID in target
  selection via the `CrackedLookup` seam, off by default. It sits beside #27 (the upload-side drop) rather
  than replacing it — the two gates act at different sites for different costs (upload TLS/queue vs.
  capture airtime/deauth).
- `HuntEngine` gains one optional read-only pointer and one setter; the frame path, the pure capture core,
  and the RX callback are untouched, so §4 #8 and #10 are preserved by construction (the lookup is never
  reachable from `onFrame`). §4 #16 is tightened, not weakened: a cracked AP is never `capturingBssid()`,
  so it is structurally never a deauth target.
- The discovered-AP *count* (and the HUD/registry) still includes cracked APs — discovery enumerates the
  whole band; gate B only removes them from the *capture* round-robin. This is deliberate: the operator
  still sees the full RF picture.
- Rotation edge, compounded: gate B is stricter than gate A about a rotated PSK. Gate A at least still
  *captures* a cracked network (it only drops the upload), so a future re-verification cadence could
  re-enable its upload; gate B skips the capture entirely, so nothing is even collected. The same deferred
  bounded re-verification cadence (ADR-0059 accepted limitation, LEDGER) must therefore also re-enable
  *targeting*, not just upload — noted on that ledger item.

## Alternatives considered

- **Filter cracked APs out of the `ApRegistry` at discovery** (never enumerate them): would also shrink the
  discovered count and hide cracked neighbours from the HUD/registry, losing the RF picture, and would put
  a persisted-state read on the discovery/append path (closer to the frame path) rather than the app-task
  selection loop. Rejected: filter at *selection*, keep enumeration honest.
- **Constructor-inject the lookup** (like the sniffer/registry): makes the persisted-state dependency
  non-optional and couples every engine construction (and every `test_hunt_engine` case) to a lookup, when
  the default and the shipped-disarmed intent is "no filter". The off-by-default setter keeps the
  frame-fed engine the baseline and the filter the addition, matching gate A and the `RawTransmitter` arm.
- **Do only gate A** (status quo after slice-0060): correct results, but keeps burning capture windows and
  deauth bursts on done-with networks — the airtime/on-air cost this ADR removes. The two gates are
  complementary, not redundant.
