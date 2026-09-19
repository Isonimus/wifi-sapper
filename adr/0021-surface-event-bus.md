---
id: '0021'
title: "Surface event bus — a synchronous multicast for engine facts"
type: architecture
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Context

ADR-0001 made surfaces observers, never owners: "The engine publishes facts ('capture
saved', 'upload ok', 'password cracked'); surfaces react," and named a ported
`core/event_bus` as the mechanism. That bus was never built. Instead each producing slice
grew the seam it needed at the time:

- **The capture→queue data path** — `CaptureReadyObserver` (ADR-0011/ADR-0017). The
  `UploadSupervisor` is the single consumer; every capture must reach it or a handshake is
  lost.
- **The sync push** — `SyncEventObserver` with `onNewPassword` / `onFirstSyncSummary` /
  `onSyncOutcome` (ADR-0019 decision #7). One observer today: `SerialSyncObserver`, which
  logs.
- **The drain pull** — `DrainOutcome` behind `UploadSupervisor::lastDrain()`, a snapshot
  getter. Its own header already anticipated this ADR: *"it is the seam slice-6/7 will turn
  into a real event subscription"* (`upload_supervisor.h`).

slice-7 is the first work with **many surfaces reacting to the same facts**: an LED now,
and a web dashboard, a screen toast, and a push webhook to follow (LEDGER). An LED status
indicator alone must react across all three of today's producers — it shows *hunting* vs
*STA window open* vs *degraded/offline* vs *new-password-recovered*, which are drain facts,
sync facts, and crack facts at once. With one consumer per seam this was direct wiring; with
N surfaces reacting to the same events it is the rule-of-three trigger (quality bar §3) for
the shared mechanism ADR-0001 always intended — and the point where growing a fourth bespoke
seam, or making every surface implement three observer interfaces and register three times,
becomes the duplication ADR-0005/ADR-0001 exist to prevent.

A decision is needed on that mechanism — its shape, its boundary against the data path, and
how the existing seams migrate onto it — before the first surface is built on top of it.

## Decision

**A single synchronous multicast `EventBus` carrying a tagged `AppEvent`.** A surface
implements one method, `EventSink::onAppEvent(const AppEvent&)`, subscribes once, and sees
every fact. This is deliberately *not* one observer interface per fact type: a status
surface reacts to facts from every producer, so one subscription that sees all of them is
what removes the "implement and register three interfaces" duplication above.

**The bus carries surface-facing facts only. The capture→queue data path stays a direct
seam.** The events on the bus are notifications a surface may react to or ignore:
`DrainStarted`, `DrainCompleted`, `SyncCompleted`, `NewPassword`, `FirstSyncSummary`. The
`CaptureReadyObserver` path — a captured handshake flowing to the `UploadSupervisor` to be
enqueued — is **not** on the bus and never will be. It is a reliable single-consumer data
pipe where a dropped or ignored event is a lost handshake; it must not become one of N
loosely-coupled subscribers whose delivery is best-effort fan-out. This boundary is the
load-bearing line of this ADR: *the bus is for facts surfaces observe, not for data the
engine must deliver.*

**Fixed capacity, no heap, synchronous, single-threaded.** `publish()` fans out to each
subscribed sink in subscription order, on the app task, before returning. There is no queue
and no deferral: the appliance is single-threaded above the Wi-Fi driver callback (§4
invariant #10 keeps that callback out of this path), and a synchronous call is the simplest
thing that works (quality bar §3). Capacity is a compile-time constant sized to the known
surface count; `subscribe()` past capacity **fails loud** — it returns `false`, and the boot
wiring treats that as fatal, exactly as `huntLoopBegin` already treats a failed mount. A
silently dropped subscription is a surface that never lights up and a bug debugged twice.

**Re-entrancy is forbidden.** A sink must not call `publish()` from within `onAppEvent()`.
Fan-out is a plain iteration over a fixed array with no re-entrancy guard, because there is
no legitimate reason for a surface to emit an engine fact — surfaces observe, they do not
produce (ADR-0001). This is a review-only rule, not a runtime guard, to keep the hot path a
bare loop.

**Payloads are borrowed for the dispatch call only.** `AppEvent` holds its type and, for the
events that carry data, a `const` pointer to the producer's live struct (`const
DrainOutcome*`, `const SyncOutcome*`, `const CrackedResult*`) plus the one scalar
`FirstSyncSummary` needs. The pointee is valid only for the duration of `onAppEvent`; a sink
that needs the data afterwards copies it. This keeps `AppEvent` tiny and heap-free and avoids
lifetime games — correct because dispatch is synchronous, so the producer's struct is still
in scope for every sink.

**The existing push seams migrate onto the bus; the pull snapshot stays for the harness.**

- `CrackedSync` publishes `SyncCompleted` / `NewPassword` / `FirstSyncSummary` to an injected
  `EventBus&` instead of calling a `SyncEventObserver&`. `SyncEventObserver` is removed;
  `SerialSyncObserver` becomes a `SerialEventLogger` EventSink that emits the same serial
  lines, now bus-sourced. `CrackedSync` stays pure and host-tested (a fake sink on a real
  bus).
- `UploadSupervisor` publishes `DrainStarted` when it opens an STA window and
  `DrainCompleted(&lastDrain_)` after each cycle, through an optional `EventBus*` (nullptr in
  an upload-only test keeps slice-0018 behaviour). It still stores `lastDrain_` and exposes
  `lastDrain()` / `lastSync()` / the count getters: those are the **pull snapshot the on-air
  verifies poll**, computed once and both published (push, for surfaces) and retained (pull,
  for the harness) — one source, no divergence. Push for surfaces, pull for tests is a
  deliberate split, not two copies of the logic.

This decision refines ADR-0019 decision #7 (which chose to *emit* sync events and named the
`SyncEventObserver` seam) by changing only the *transport* — the same three facts, now on the
shared bus. It does not change what is emitted or when.

## Consequences

- **Adding a surface is additive, as ADR-0001 promised:** implement `EventSink`, subscribe in
  the boot wiring, touch no engine code. The LED (slice-0022), and later the web dashboard,
  screen toast, and push webhook, each cite this ADR and add exactly one sink.
- **The data path and the fact stream are now visibly different kinds of thing.** A future
  contributor cannot accidentally route a capture through the best-effort bus, or a status
  fact through the must-deliver capture seam — the boundary is stated here and enforced by §4
  invariant #13.
- **The migration reworks freshly-shipped slice-6/slice-18 wiring** (`CrackedSync`'s
  constructor, `hunt_loop`'s observer, the supervisor's outputs) and their host tests. The
  cost is paid once, now, deliberately (the alternative — deferring the bus and fanning out
  from bespoke seams — was considered and rejected: it re-litigates ADR-0001 and refactors on
  the second surface instead of the first). The existing host lanes are the regression net:
  each migrated seam keeps its behavioural tests, retargeted from the removed observer to a
  fake sink.
- **A surface that needs an event's data beyond the callback must copy it.** The LED latches
  a copied state, not the borrowed pointer. This is documented on `AppEvent`; a surface that
  stashes the pointer reads freed/overwritten memory — caught at review, the one hazard the
  borrowed-payload choice introduces in exchange for zero heap.
- **`subscribe()` capacity is a fixed constant.** A fifth/sixth surface beyond the reserved
  count is a one-line bump with a re-measure of the (tiny) static cost; exceeding it at
  runtime fails loud at boot rather than silently dropping a surface.
- Adds §4 standing invariant #13, in this same commit: surfaces receive engine facts only by
  subscribing to the `EventBus`, and the engine never calls a surface directly.
