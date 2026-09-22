---
id: '0062'
title: "Skip targeting already-recovered APs in the hunt (gate B of ADR-0059)"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0061 gate B: `HuntEngine` skips an already-cracked AP in target selection, reusing the
read-only `CrackedLookup` seam slice-0060 introduced, off by default so the engine stays purely frame-fed
when no lookup is wired.

- **`HuntEngine`:** an optional `const CrackedLookup* crackedLookup_ = nullptr` set by
  `void setCrackedLookup(const CrackedLookup&)` (mirrors `UploadSupervisor::setCrackedLookup` and the
  optional `RawTransmitter`). In `doStartCapturing`, before `retuneTo`, if the lookup is set and reports
  `registry_.at(targetIndex_).bssid` cracked, `++targetIndex_; continue;` — the same skip the
  channel-rejected case uses. `advanceTarget` funnels through the same loop, so one insertion covers the
  round-robin; when all targets are skipped the loop falls through to `doEnterDiscovering` (a fresh sweep).
- **Wiring (`hunt_loop.cpp`):** `g_engine->setCrackedLookup(g_manifest);` after the engine is constructed
  (the manifest is already `begin()`-loaded and wired to the supervisor) — a device-only one-liner
  (review-only).
- **§4:** add invariant #28 (targeting-side skip) in the same commit as ADR-0061; update the ADR-0059
  rotation-limitation LEDGER item to note gate B compounds it.

## Definition of Done

**Scenario A — a discovered-but-cracked AP is never targeted; an uncracked one is.**
- **Given** discovery that enumerates AP X (cracked) and AP Y (not), with a `CrackedLookup` reporting X
  cracked and the lookup set on the engine
- **When** the engine runs its discover→capture cycle and both APs' EAPOL is available
- **Then** the engine captures Y (raises `onCaptureReady` for Y) and **never enters Capturing for X**
  (X is never `capturingBssid()`, so it is also never deauthed) — the round-robin skips X.
- **Proof:** `test/test_hunt_engine` — fails before (X is captured too), passes after. See `## As built`.

**Scenario B — with no lookup set, every discovered AP is targeted (unchanged, purely frame-fed).**
- **Given** the same discovery but `setCrackedLookup` never called (`nullptr`)
- **When** the cycle runs
- **Then** both X and Y are captured — the slice-0016 target selection is untouched and §4 #8/#10 hold.
- **Proof:** `test/test_hunt_engine` (the pre-existing round-robin tests still pass unchanged, plus an
  explicit no-lookup case). See `## As built`.

**Scenario C — all discovered APs cracked degrades to re-discovery, never a spin.**
- **Given** discovery enumerates only cracked APs, lookup set
- **When** the post-discovery capture phase is reached
- **Then** the engine captures nothing and returns to Discovering (a fresh sweep), rather than stalling
  or busy-looping — the existing "no capturable target remained" fall-through.
- **Proof:** `test/test_hunt_engine` — the engine re-enters Discovering with no capture reported. See
  `## As built`.

## Verification

- **Target-selection skip (Scenarios A, B, C)** — `pio test -e native` (`test_hunt_engine`),
  fail-before/pass-after recorded in `## As built`. No new verify script: the behaviour is fully
  host-assertable (a fake sniffer + fake registry population + a fake lookup), like gate A — unlike an
  on-air proof.
- **Device wiring (`hunt_loop` `setCrackedLookup`)** — review-only: a one-line injection of `g_manifest`,
  no new runtime path to drive headlessly.

## As built

Shipped exactly as designed. `HuntEngine` gained `void setCrackedLookup(const CrackedLookup&)` and a
`const CrackedLookup* crackedLookup_ = nullptr` member (`src/net/hunt_engine.h`); the skip is one guard in
`doStartCapturing`'s selection loop, before `retuneTo`, treating a cracked target like a channel-rejected
one (`++targetIndex_; continue;` — `src/net/hunt_engine.cpp`). `hunt_loop.cpp` wires
`g_engine->setCrackedLookup(g_manifest)` right after gate A's `g_supervisor->setCrackedLookup(g_manifest)`,
before `g_engine->begin()`, so gate B is live from the first discovery cycle.

- **Scenarios A, B, C — `test/test_hunt_engine`** (`test_cracked_ap_is_never_targeted_but_uncracked_one_is`,
  `test_with_no_lookup_every_ap_is_targeted`, `test_all_cracked_degrades_to_rediscovery_not_a_spin`), driven
  through the real state machine via the fake sniffer + a `FakeCrackedLookup`.
- **Fail-before/pass-after:** neutralising the skip (`if (false && …)`) failed A (`Expected TRUE Was FALSE`
  — the cracked AP was captured) and C (`Expected 1 Was 2` — nothing was skipped, so the registry was not
  re-swept), while B stayed green (it exercises the unset-lookup path). Restored → all 19 `test_hunt_engine`
  cases and the full 282-case native suite pass; `cardputer` firmware builds; lint 0/0; INDEX byte-identical
  to the generator.
- **Adversarial pass (Sonnet 5, blind to intent, aware of law — diff + CLAUDE.md + INDEX.md, read the files
  itself, no recursion):** no real defects. It independently reproduced the fail-before/pass-after and
  confirmed the lookup is read only on the app task in `doStartCapturing` (never in `onFrame`), so §4 #8/#10
  hold; that `crackedLookup_` is set once before the tick loop and never reassigned, and `g_manifest` is
  mutated only on the same cooperative main-loop task, so no lock is needed (gate A's justification carries
  over); and that a cracked AP, never becoming `capturingBssid()`, is structurally never deauthed (§4 #16).
