---
id: '0033'
title: "Live hunt HUD: an in-flight-capture pull snapshot the screen polls, never a bus event"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

The on-device status HUD (ADR-0025) shows cumulative counters and the last drain/sync/crack — a
record of what *has* happened. It says nothing about what the appliance is doing *right now*: which
network it is parked on and how much of that handshake it has collected. An operator watching the
panel during a hunt sees a live-looking HUD (the heartbeat blinks) but no live *content* — the same
"is it actually working?" gap the heartbeat was added to answer, one level up. The sibling interactive
`../adversary` solves this on its capture screen with a per-message indicator row (Beacon, M1–M4) and
a progress bar; the Sapper wants the same, adapted to the headless-first surface model.

The data already exists inside the engine: while Capturing, `HuntEngine` holds a `HandshakeCollector`
whose `CapturedHandshake` carries the target `bssid`/`ssid` and which of Beacon/M1–M4 have arrived,
and it exposes `phase()`, `parkedChannel()`, `capturingBssid()`, `discoveredCount()`. The question is
how a surface reads live, high-frequency engine state without violating the boundaries the surface
model rests on.

Two of those boundaries bound the design:

- **§4 invariant #13** already says a surface reaches engine state "only … [via] the pull snapshot the
  on-air verifies use", and keeps the *bus* for discrete, must-not-spam facts. Live in-flight state
  (message flags flipping as frames arrive, changing many times per capture) is exactly what must NOT
  go on the synchronous bus — publishing per frame would turn every surface's `onAppEvent` into a hot
  path. So this is a *pull*, not a push. This ADR makes that sanctioned pull concrete.
- **§4 invariant #10** keeps the promiscuous RX callback (the driver task) doing nothing but forwarding
  raw bytes to the pure core. The collector is therefore *written* on the driver task (`onFrame` →
  ingest), while a HUD read happens on the app task (`tick`). A read cannot take a lock the driver-task
  hot path would then contend on without violating #10's "no blocking in the callback context".

A second problem is proving it on hardware. The deterministic on-air proof (the operator chose the
fuller option) needs the engine to actually be Capturing with a populated collector — but the existing
`huntLoopInjectStimulus` bypasses the engine's collector (it builds its own and calls the capture-ready
relay). Lighting the indicators on a real panel deterministically requires driving synthetic frames
through the engine's *own* receive path.

## Decision

1. **A read-only pull snapshot, `HuntSnapshot`, behind a `HuntSnapshotSource` seam**
   (`net/hunt_snapshot.h`): a POD carrying `phase`, `channel`, `discovered` (count), the target
   `bssid`/`ssid`, and `hasBeacon`/`hasM1`..`hasM4`. `HuntEngine` implements
   `HuntSnapshotSource::huntSnapshot()` by reading its own live state (`phase_`, `parkedChannel_`,
   `registry_.count()`, `collector_.handshake()`). This is the concrete form of the pull #13 already
   blesses — the surface reads engine state through one narrow, const seam, never by befriending the
   engine's internals.

2. **The screen surface pulls each tick; live hunt state never rides the bus.** `ScreenToastSurface`
   gains an optional `HuntSnapshotSource*` (set post-construction, like the webhook's notifier) and, in
   `tick()`, folds the snapshot into its `ScreenView`. The bus is untouched: no new `AppEventType`,
   because live per-frame state would spam the synchronous dispatch (§4 #13). A surface with no source
   wired simply renders the pre-0033 HUD. → new invariant **#18**.

3. **The read is deliberately best-effort/cosmetic, and takes no lock.** The collector is written on
   the driver task; `huntSnapshot()` reads it on the app task. Each field read is safe without a lock,
   for a *specific* reason rather than a blanket "it's stable" claim:
   - `bssid` is written once, at retarget, on the *app* task under the §4 #11 quiesce settle — no
     driver-task write races it.
   - the presence flags are backed by a small length field written monotonically (0 → set once per
     frame, never reset until retarget), so a torn read is at worst a one-tick-early/late indicator.
   - `ssid` *is* re-written by every beacon for the current target on the driver task, so it is the one
     field a naive in-place clear-then-fill would expose empty mid-write. The collector therefore
     publishes it in a **single memcpy of a complete value** (built in a scratch first), so a concurrent
     read sees a consistent name, never a transient empty one; and the surface printable-filters it so a
     torn byte cannot corrupt the drawn line. Worst residual case is a one-tick-stale name.

   No lock is added: guarding the collector for a cosmetic read would put contention on the driver-task
   hot path, which #10 forbids. #13's own words — "a dropped fact is cosmetic" — govern here.

4. **Per-phase rendering** (mirrors `../adversary` `handshake_screen.cpp`): Discovering →
   `SCAN ch<N> · <count> seen`; Capturing → the target's SSID (or BSSID when hidden) + a B/M1/M2/M3/M4
   indicator row (present = highlighted) + a progress bar filled by the count present of those five;
   Idle/Quiescing → a resting line. The renderer only draws what the `ScreenView`'s new hunt fields
   say; the surface decides them from the snapshot.

5. **A test-only frame-injection stimulus makes the on-air proof deterministic.** A
   `SAPPER_TEST_HOOKS`-gated `huntLoopInjectHudStimulus()` builds a synthetic beacon + M1/M2 (reusing the
   existing stimulus builders) and feeds each through the engine's `onFrame` — the *exact* seam a real
   sniffed frame uses (§4 invariant #2: injected stimulus enters only through the real seam, never a
   parallel collector-poking path). It is compiled out of every shipped build and exposed by no serial
   command (§4 invariant #4). The beacon makes the engine discover and target the synthetic AP; an M1
   then lands in its collector while it captures that BSSID, so the panel deterministically shows Beacon
   + M1 lit (progress 2/5). It is a *partial* handshake on purpose — a wpa-sec-valid set (beacon+M1+M2)
   would make the engine report-and-advance at once, so the populated HUD would flash by rather than sit
   still for the panel `dump`; beacon+M1 is not valid, so the engine stays on the target for its capture
   window with a stable, dumpable HUD. The probe also defers the cracked-sync (via a test hook that
   pushes its deadline out) so no STA window opens to stop the engine on a network-less bench, and runs
   on a quiet channel so the synthetic AP is the one captured.

## Consequences

- `net/hunt_snapshot.h` is new (POD + seam); `HuntEngine` gains `huntSnapshot()` and the
  `HuntSnapshotSource` base. `ScreenView` gains a hunt section (phase/channel/discovered/target/flags)
  compared in `operator==`, and `ScreenToastSurface` a `setHuntSource()` + the per-tick fold. The device
  `Esp32ScreenRenderer` draws the indicator row + progress bar.
- New §4 invariant **#18** records that live in-flight hunt state reaches a surface only via the
  read-only `HuntSnapshot` pull (app-task, best-effort), never the `EventBus`. Added in this ADR's
  commit (the same-commit rule). It refines, and does not supersede, #13.
- `hunt_loop` gains a `SAPPER_TEST_HOOKS`-only `huntLoopInjectHudStimulus` (the beacon/EAPOL builder is
  shared with the upload stimulus, not duplicated); a new hunt-HUD probe (`SAPPER_TEST_HUNT_HUD`) and
  `scripts/0034-hunt-hud-verify.mjs` prove the lit HUD on hardware. The shipped binary carries neither (#4).
- ADR-0025 is extended, not superseded: the HUD gains a live section; its counter/toast semantics are
  unchanged.
- The cosmetic-read decision is a deliberate deviation a later reader might try to "fix" with a lock;
  it is cited at the `huntSnapshot()` site so the choice announces itself (stele:ADR-0012).
