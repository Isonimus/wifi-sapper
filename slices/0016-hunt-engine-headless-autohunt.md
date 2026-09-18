---
id: '0016'
title: "HuntEngine — headless endless-AutoHunt state machine"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Turn the capture pieces into the endless loop the appliance is. A pure `HuntEngine` (ADR-0015)
orchestrates the existing seams — `RadioSniffer`, `ChannelHopper`, `ApRegistry`, `HandshakeCollector`
— through the states Discovering → Capturing (round-robin over the discovered APs) → Discovering,
forever. It is itself the sniffer's single stable `FrameConsumer` and routes each frame through one
atomic pointer to the sink the current phase uses, so the consumer never changes under the running
radio. It sequences every sink clear through a non-blocking quiesce settle, discharging the two
cross-task audit items the ledger parked on it (the collector `stop()` race, ADR-0011; the
`ApRegistry::reset()` sequencing, ADR-0013). A wpa-sec-valid capture leaves through a capture-ready
event.

It does **not** upload, connect as a station, or drive any display (headless — ADR-0001; the event is
where slice-5/7 attach); does **not** build the promiscuous-vs-STA arbitration (no uploader to switch
to yet — moved to slice-5, ADR-0015 decision #7); does **not** transmit (no deauth — deferred), so on
air it proves the *loop mechanics*, not a live handshake capture (that needs a deauth and a pcap sink
— slice-5, ledger). There is still no shipped hunt loop: the device wiring is bench scaffolding behind
`SAPPER_TEST_HOOKS` (`hunt_probe`), existing only so the lane-3 verify has firmware to drive
(ADR-0015 decision #8).

## Definition of Done

**Scenario A — the engine sweeps to discover, then captures a target, then advances (host).**
- **Given** a `HuntEngine` wired to a fake `RadioSniffer` and a fake clock, over the hop list
  `{1, 6, 11}`
- **When** it is `tick`ed with a rising clock while the fake sniffer delivers beacons for two distinct
  APs during the discovery window, then EAPOL frames for the first target during its capture window
- **Then** it sweeps across the hop channels during Discovering (the fake sniffer records the
  `setChannel` calls), snapshots the two APs, retunes to the first target's channel and enters
  Capturing with the collector as the active sink, and on a wpa-sec-valid handshake raises the
  capture-ready event exactly once for that BSSID and advances to the second target — driven purely by
  the clock and injected frames, no radio.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario B — round-robin visits every discovered AP and then re-discovers (host).**
- **Given** a `HuntEngine` that discovered three distinct APs
- **When** it is ticked through the capture window of each in turn (no handshake arriving, so each ends
  on its per-target timeout)
- **Then** it captures each of the three distinct targets exactly once, in turn, and after the third
  returns to Discovering rather than repeating a target — the endless loop cycles the whole
  enumeration.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario C — a target whose channel the radio rejects is skipped, not captured wrong (host).**
- **Given** a `HuntEngine` with two discovered targets, and a fake `RadioSniffer` configured to reject
  `setChannel` for the first target's channel (a country-restricted channel)
- **When** it advances to that target
- **Then** it does not enter Capturing on the un-retuned radio: it skips to the next target (whose
  channel the radio accepts) and raises no capture for the skipped BSSID — never collecting on a
  channel it did not reach (ADR-0013 / ADR-0015 decision #5).
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario D — a sink is cleared only after the quiesce settle, never while active (host).**
- **Given** a `HuntEngine` transitioning between phases that must clear a sink (Capturing→Discovering
  clears the registry; target→target re-targets the collector)
- **When** it is ticked across the transition with the fake clock
- **Then** the active-sink pointer is aimed at `nullptr` for at least the settle interval before the
  sink is `reset()`/`retarget()`ed and the pointer re-aimed — a frame delivered mid-transition reaches
  no sink under reset — and the settle is measured from the clock, not a blocking wait.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario E — an on-air hunt loop discovers, selects, retunes, and cycles (on-air, lane 3).**
- **Given** a Cardputer flashed with the `cardputer_testhooks` build, `SAPPER_TEST_HUNT` enabled with
  a hop list (e.g. `1,6,11`), and live APs in range
- **When** `npm run verify:hunt` drives the device
- **Then** the device announces the hunt (`[HUNT] discovering …`), discovers at least one real AP
  (`[HUNT] target bssid=… ssid=… channel=…`), retunes to a selected target and enters capture
  (`[HUNT] capturing bssid=… channel=…`), and advances/returns to discovery (`[HUNT] discovering …`
  again) — proving the loop cycles; the run fails on any `[FATAL]`/`[ERROR]` line and writes the
  observed `[HUNT]` lines as its artifact, which also records the observed quiesce/phase timing. No
  live handshake is asserted — that needs a deauth and a pcap sink (slice-5, ledger).
- **Proof:** `scripts/0016-hunt-verify.mjs`, wired as `npm run verify:hunt`.

## Design

**The engine (pure).** `src/net/hunt_engine.{h,cpp}` — a `FrameConsumer` holding a `RadioSniffer&`, an
`ApRegistry&`, an owned `HandshakeCollector`, a `ChannelHopper`, the current `Phase`, phase
deadlines, the round-robin index and the captured-target snapshot, and the `std::atomic<FrameConsumer*>
activeSink_`. `begin(nowMs)` installs the engine as the sniffer's consumer on the first hop channel
and enters Discovering. `tick(nowMs)` runs the state machine: sweep + registry during Discovering;
snapshot + select + retune into Capturing; raise the capture-ready event on `isWpaSecValid()`; advance
round-robin; quiesce (aim `activeSink_` at `nullptr`, wait the settle, then clear/re-target) on every
transition that clears a sink. `onFrame` (driver task) only `load`s `activeSink_` and forwards. No
`Serial`, no radio in the logic — the injected seam is the only hardware path (ADR-0015 decisions
1–5). Host-compiled on the native lane.

**The capture-ready event (pure seam).** An allocation-free observer — a `CaptureReadyObserver`
interface (or function-pointer-plus-context) the engine calls from `tick` with the
`CapturedHandshake` when a target first becomes wpa-sec-valid (ADR-0015 decision #6). The engine holds
it by reference; the probe's implementation prints the capture, and slice-5's uploads it.

**The collector re-target (pure).** `HandshakeCollector::retarget(bssid, channel)` — clears the
accumulated frames and points the collector at a new BSSID/channel, so one collector serves the whole
round-robin without a multi-KB value copy or heap churn (ADR-0015 consequences). `reset()` (same
target) stays for the single-target case.

**The quiesce settle (pure, clock-driven).** A `Quiescing` phase carrying the settle deadline and the
pending next phase; `tick` completes the clear only once `nowMs` passes the deadline (ADR-0015
decision #4). Default settle is a build-time parameter; the callback it outlasts is a bounded memcpy,
so the number is an architecture-derived ceiling and its measured floor is a ledger follow-up citing
ADR-0015 — the verify artifact reports the observed timing, as slice-0014's does for the dwell.

**The seam (device).** No new seam method — the engine drives the existing `begin`/`setChannel`/`stop`
(ADR-0015 decision 1). The engine being the single stable consumer is what makes `stop()`'s inability
to hard-join safe (§4 invariant #11, added by ADR-0015).

**The bench probe (device-only, `SAPPER_TEST_HOOKS`).** `src/hunt_probe.{h,cpp}` — when
`SAPPER_TEST_HUNT` is set, it parses the hop list (and optional windows/settle overrides), wires an
`Esp32RadioSniffer` + `ApRegistry` + `HuntEngine` with a print-only `CaptureReadyObserver`, calls
`begin`, and pumps `tick(millis())`, emitting `[HUNT] discovering/target/capturing …` on phase
changes. Empty var → inactive; no `SAPPER_TEST_HOOKS` → no-op stubs, so no shipped binary hunts
(ADR-0015 decision #8). `main.cpp` gives it first refusal at boot, before the slice-0014 discover
probe.

## Verification

- **Native Unity tests** (Scenarios A–D) — `test/test_hunt_engine` drives the engine through an
  injected fake `RadioSniffer` (records `setChannel`, lets the test push frames to the installed
  consumer) and a fake clock: the discover→capture→advance loop, the full-enumeration round-robin, the
  retune-failure skip, the capture-ready event firing once per valid target, and the quiesce settle
  timing. Wired as `npm run test:native` (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1).
  The atomic router's real-time coherence and the on-air loop are not host-assertable and are proved
  by Scenario E.
- **On-air verify** (Scenario E) — `scripts/0016-hunt-verify.mjs`, wired as `npm run verify:hunt`
  (R11). Runs on a workstation with the board attached, never in cloud CI (§4 invariant #5); its
  error-check half (fail on any `[FATAL]`/`[ERROR]`, require a discovered target, a capture entry, and
  a return to discovery) is machine-checkable, and it writes the observed `[HUNT]` lines as the review
  artifact, recording the observed phase/quiesce timing (the settle measurement — ADR-0015 decision
  #4).
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) links the shipped
  `.elf`; `hunt_engine.cpp` shares the native lane and compiles under the ESP toolchain, and
  `hunt_probe.cpp` compiles to no-op stubs in the shipped build. Nothing references the engine there
  (the stubs return false), so the linker drops it — the shipped binary carries no `HuntEngine`
  symbol and its RAM is unchanged; Flash grows only by the inactive probe stubs and the one boot
  branch that skips them, never by hunt state.

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each Definition-of-Done
scenario was met or recording the deviation.)_
