---
id: '0015'
title: "HuntEngine — the headless endless-AutoHunt state machine over the capture seams"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

Every piece the autohunt needs now exists and is host-tested, but nothing drives them: the
`RadioSniffer` seam (`begin`/`setChannel`/`stop`, ADR-0011), the pure `ApRegistry` that enumerates
APs from beacons (ADR-0013), the pure `HandshakeCollector` that accumulates one target's 4-way
handshake (ADR-0009), and the `ChannelHopper` schedule (ADR-0013). This ADR records the shape of the
component that orchestrates them into the endless loop the appliance is: sweep to discover the APs in
range, pick one, retune to it and capture its handshake, raise the capture for whoever uploads it,
and move on — forever. It is the "extract the endless AutoHunt state machine out of the UI into a
headless engine" the ledger has carried as slice-4 since the project's scope was set (ADR-0001).

Three forces shape the design, and one of them is the reason this component — not any earlier one —
exists:

- **The sniffer seam holds exactly one consumer for its started lifetime, and `stop()` cannot
  hard-join a callback already in flight (ADR-0011; §4 invariant #10 forbids blocking in the driver
  callback).** Discovery and capture want *different* consumers (an `ApRegistry` vs a
  `HandshakeConsumer`). The obvious "swap the consumer" — `stop()` then `begin()` with the new one —
  is unsafe: a frame dequeued just before `stop()` can still run `onFrame()` on the old consumer
  after `stop()` returns, and re-`begin()` racing that is undefined. Both audit items the ledger
  parked *on this engine* (the collector `stop()` race, ADR-0011; the `ApRegistry::reset()`
  sequencing, ADR-0013) are the same hazard: a sink read or cleared while the driver task may still
  be writing it.

- **The endless loop must re-use bounded memory, so its sinks must be cleared and re-aimed
  repeatedly** — the registry between discovery rounds (it is bounded at `kMaxDiscoveredAps` and
  reports `overflowed()`, not grows), the collector between targets. Clearing a sink is exactly the
  cross-task write the first force makes unsafe to do naively.

- **Headless-first (ADR-0001): the capture core has no UI dependency and every surface is an event
  subscriber.** The engine captures; it must not upload, connect as a station, or drive a screen.
  What it produces — a wpa-sec-valid handshake — leaves through an event, and the uploader
  (slice-5), the alerter (slice-7), and any display subscribe to it.

## Decision

**1. The engine is a pure, tick-driven state machine that is itself the sniffer's single, stable
consumer.** `HuntEngine` implements `FrameConsumer` and is the one consumer passed to
`sniffer.begin()` for the whole run. It never changes. All state-machine work happens in
`tick(nowMs)`, called from the app loop with the caller's clock — no internal timer, no task — so a
host test drives the entire loop deterministically against a fake clock and an injected fake
`RadioSniffer`, exactly as the `ChannelHopper` is tested (ADR-0013 decision #1). The radio never
reaches the pure logic; only the injected seam does.

**2. `onFrame` routes each frame, through one atomic sink pointer, to the pure sink the current phase
uses.** The engine holds a `std::atomic<FrameConsumer*> activeSink_`. `onFrame` (driver-task context)
does nothing but `load(acquire)` that pointer and forward — a bounded memcpy into whichever sink,
within §4 invariant #10, the same budget the collector's `ingest` already runs in (ADR-0011 decision
#3). The state machine (app task) aims the pointer at the `ApRegistry` during discovery and at a
`HandshakeConsumer` during capture. This replaces the unsafe "swap the sniffer's consumer" with an
atomic store the driver task reads coherently: the sniffer's consumer is *always* the engine, and a
frame arriving at the instant of a switch is delivered to a fully-constructed sink that ignores what
does not concern it (the registry drops non-beacons and duplicates; the collector drops foreign
BSSIDs). No frame reaches a half-built or destroyed object.

**3. The phases are Discovering → Capturing (round-robin over the enumeration) → Discovering,
endlessly, with an explicit non-blocking Quiescing transition between them.** In *Discovering* the
sink is the registry and the `ChannelHopper` sweeps (`tick` retunes via `setChannel` and updates the
registry's fallback channel only after a confirmed retune — ADR-0013 decision #3). After a bounded
discovery window the engine snapshots the enumeration and enters *Capturing* the first target. In
*Capturing* the sink is the collector for the selected BSSID, the radio is parked on the target's
channel, and on the first `isWpaSecValid()` the engine raises the capture-ready event (decision #6)
and advances; a bounded per-target window also advances. When the round-robin is exhausted the engine
returns to Discovering. Every transition that must clear a sink passes through *Quiescing* (decision
#4).

**4. Both audit items are discharged by one non-blocking quiesce protocol, chosen over
double-buffering for memory.** To clear a sink (the registry before a new discovery round, the
collector before a new target) or to tear down, the engine: (a) stores `nullptr` into `activeSink_`
so new frames are discarded; (b) enters *Quiescing* and waits a bounded settle measured by the same
`tick` clock — non-blocking and host-testable, never a `delay()` in the callback the invariant
forbids; (c) once the settle elapses, no driver-task callback can still be inside the old sink, so it
`reset()`s (or re-targets) the sink and re-aims the pointer. The settle replaces the hard-join the
seam cannot give. Double-buffering would avoid the wait but the `HandshakeCollector` is multiple KB
(six whole frames); a second copy is memory a no-PSRAM ESP32-S3 should not spend to save a few
milliseconds per phase switch on an endless background loop. The settle is a parameter with a
conservative default; the callback it outlasts is a bounded memcpy of microseconds, so the true floor
is a hardware measurement, tracked in the ledger against this ADR as the hop dwell is (ADR-0013
decision #5) — the default is an architecture-derived ceiling on a microsecond callback, not yet a
measurement.

**5. Target selection is round-robin over the discovered enumeration; a target the radio cannot
retune to is skipped, not captured on the wrong channel.** With no RSSI (ADR-0013 decision #6 defers
it — it would widen the raw-bytes seam), the engine has no ranking signal, so it hunts every distinct
AP in turn: an unattended appliance should attempt every network in range, not fixate on one. Before
capturing, it `setChannel`s to the target's channel; if the radio rejects it (country-restricted, as
`setChannel` can fail — ADR-0013), the engine skips that target rather than collecting on a channel
it never reached — the same "never a guessed channel" discipline the registry's fallback follows
(ADR-0013 decision #3). RSSI-based prioritisation slots in later as the ledger already records.

**6. A wpa-sec-valid capture leaves through a capture-ready event on the app task; the engine never
uploads.** The engine takes a callback (a `std::function`-free function-pointer-plus-context, or a
small observer interface — an allocation-free seam) invoked from `tick` with the `CapturedHandshake`
the instant a target first becomes `isWpaSecValid()`. It fires on the app task, never in the driver
callback (heavy work stays off the callback — §4 invariant #10). This is the headless event boundary
(ADR-0001): the uploader (slice-5), the alerter (slice-7), and any display subscribe here; the engine
knows nothing of them. The engine does not connect as a station, open a socket, or touch the display.

**7. The promiscuous-vs-STA radio arbitration is *not* built in this slice — it moves to slice-5.**
ADR-0013 decision #6 named the HuntEngine layer as the owner of the promiscuous-vs-STA switch. That
ownership stands; this ADR settles *when* it is built, and it is not now. There is no uploader, so
the STA mode the arbiter would switch into does not exist — building the arbiter here is speculative
generality (quality bar §3): a second radio mode with no caller and nothing to test the switch
against, which slice-5 would then have to reshape around the real socket lifecycle. The engine
instead emits the capture-ready event and keeps sniffing; slice-5 introduces the uploader *and* the
arbitration that pausing the hunt to bring the STA up requires, with both modes in hand. The ledger's
target-selection line is re-pointed from slice-4 to slice-5 for the arbitration half, citing this ADR
(a ledger edit — single writer, one direction; ADR-0013's body is not touched).

**8. No shipped hunt loop yet.** The pure engine compiles into every build (it is host-tested code
with no radio dependency), but nothing *starts* it in a shipped binary: with no uploader, an endless
hunt would capture handshakes with nowhere to send them. The device wiring — construct the
`Esp32RadioSniffer`, the engine, the registry, run the loop — is bench scaffolding gated behind
`SAPPER_TEST_HOOKS` (`hunt_probe`), driven by the slice-0016 verify; it compiles to no-op stubs in
every shipped build, so no shipped binary hunts. Sniffing and hopping are receive-only, so this is
not the deauth/upload actuation of §4 invariant #4, but the gating keeps the scaffolding out of every
shipped binary, as ADR-0011 decision #5 and ADR-0013 decision #7 did for their probes. The shipped
loop lands when slice-5 gives the capture somewhere to go.

## Consequences

- A new §4 invariant (#11) is added in this commit: a consumer is switched under a live
  `RadioSniffer` only by the atomic router the engine holds for the sniffer's lifetime, and a sink is
  reset or destroyed only after a quiesce settle with the router aimed away from it — never by a
  `stop()`/`begin()` re-attach, which cannot hard-join an in-flight callback (ADR-0011). This is a
  genuinely new obligation the existing invariants do not cover (#8 is the raw-bytes seam boundary,
  #10 is the no-work-in-callback budget); the deauth and station-scanner slices, which register their
  own consumers on the same seam, inherit it. Enforced review-only: it is a concurrency discipline a
  unit test cannot assert, which is what the lane-3 verify and `/wrap-up` cover.
- `HandshakeCollector` gains a `retarget(bssid, channel)` method so the engine can re-aim one
  collector across the round-robin without heap churn or a multi-KB value copy per target; `reset()`
  kept the same target, which the endless loop cannot use. It is a pure addition, host-tested.
- The engine's correctness splits cleanly across the verification lanes (ADR-0004): the state-machine
  logic — phase transitions, round-robin order, the capture-ready event, the retune-failure skip, the
  quiesce timing against a fake clock — is host-tested on the native lane through an injected fake
  `RadioSniffer` (invariant #2: stimulus enters through the same `onFrame` a real frame uses); the
  real-time quiescence and the on-air loop are proved by the slice-0016 lane-3 verify. A *real* 4-way
  handshake on air still needs a deauth to force renegotiation (deferred) and a byte sink to write the
  pcap (slice-5), so the lane-3 verify proves the loop *mechanics* (discovers, selects, retunes,
  cycles), not a live capture — that proof belongs to slice-5, where both exist (ledger).
- The capture-ready event is the integration seam slices 5–7 attach to; fixing its shape now (an
  allocation-free observer carrying the `CapturedHandshake`) means the uploader and alerter subscribe
  rather than re-plumb the engine.
