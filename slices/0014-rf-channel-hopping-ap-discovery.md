---
id: '0014'
title: "RF channel hopping and beacon-based AP discovery"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Turn the fixed-channel sniffer (slice-0012) into a band sweeper that enumerates the APs in range —
the input a targeted hunt needs. Two pure components over the existing seam (ADR-0013): a
`ChannelHopper` that schedules which channel to sit on, and an `ApRegistry` that records the beacons
each channel visit yields (BSSID, SSID, operating channel). The one new hardware capability is
`RadioSniffer::setChannel()`, the retune the hopper drives.

It does **not** select or rank targets (the registry enumerates; choosing which AP to hunt, and the
promiscuous-vs-STA radio switch, belong to the HuntEngine, slice-4), does **not** rank by signal
(RSSI would widen the raw-bytes seam — ADR-0013 decision #6), does **not** transmit, and does
**not** persist or upload. There is still no shipped sweep loop: the device hop-and-discover path is
bench scaffolding behind `SAPPER_TEST_HOOKS` (`rf_discover_probe`), existing only so the lane-3
verify has firmware to drive (ADR-0013 decision #7).

## Definition of Done

**Scenario A — the channel hopper sweeps on schedule (host).**
- **Given** a `ChannelHopper` over the list `{1, 6, 11}` with a 300 ms dwell, started at t=0
- **When** it is ticked with a rising clock
- **Then** it stays on channel 1 until the dwell elapses, advances 1→6→11 at each 300 ms boundary
  and not before, and wraps 11→1 — driven purely by the clock, with no radio.
- **Proof:** `npm run test:native` → `test/test_channel_hopper`.

**Scenario B — the AP registry enumerates distinct APs from beacons (host).**
- **Given** an `ApRegistry` set to a current sweep channel
- **When** beacons for several distinct BSSIDs are delivered through `onFrame()` — the seam entry
  point the device callback calls — including a duplicate BSSID, a beacon whose DS Parameter Set
  advertises a channel differing from the sweep channel, a beacon with no DS Parameter Set IE, and a
  non-beacon frame interleaved
- **Then** the registry lists each distinct BSSID exactly once with its SSID; records the DS-Param
  channel where present and the current sweep channel as the fallback where absent; and ignores the
  non-beacon frame. This proves discovery reaches the pure core through the real consumer, no radio.
- **Proof:** `npm run test:native` → `test/test_ap_registry`.

**Scenario C — `beaconChannel` reads the DS Parameter Set element (host).**
- **Given** raw beacon frames — one carrying a DS Parameter Set IE, one without it, one with a
  truncated IE list
- **When** `beaconChannel()` is called on each
- **Then** it returns the advertised channel for the first and 0 (unknown) for the other two — never
  a guess. This is the parser Scenario B's channel column depends on.
- **Proof:** `npm run test:native` → `test/test_eapol`.

**Scenario D — an on-air sweep hops across channels and discovers real APs (on-air, lane 3).**
- **Given** a Cardputer flashed with the `cardputer_testhooks` build, `SAPPER_TEST_RF_HOP` set to a
  channel list (e.g. `1,6,11`), and live APs in range
- **When** `npm run verify:rf-discover` drives the device
- **Then** the device announces the sweep (`[HOP] sweeping …`), retunes across at least two distinct
  channels (`[HOP] channel=…`), and reports at least one discovered AP with a BSSID, SSID, and
  channel (`[DISCOVER] ap bssid=… ssid=… channel=…`); the run fails on any `[FATAL]`/`[ERROR]` line
  and writes the observed lines as its artifact, which also records per-channel reception at the
  configured dwell (the dwell measurement — ADR-0013 decision #5).
- **Proof:** `scripts/0014-rf-discover-verify.mjs`, wired as `npm run verify:rf-discover`.

## Design

**The hopper (pure).** `src/net/channel_hopper.{h,cpp}` — holds a copied channel list, a dwell, an
index, and the last-hop timestamp. `currentChannel()` returns the channel to be on;
`tick(nowMs)` advances to the next channel (wrapping) and returns true when the dwell has elapsed,
false otherwise; `reset(nowMs)` re-parks on the first channel. No radio, no `Serial`, no clock of its
own — the caller supplies `now`, so a test drives the whole schedule deterministically (ADR-0013
decision #1). Host-compiled on the native lane.

**The registry (pure).** `src/net/ap_registry.{h,cpp}` — a `FrameConsumer` whose `onFrame` parses a
beacon and records each distinct BSSID once into a bounded `DiscoveredAp[]`, with SSID and channel
(`beaconChannel` where the DS-Param IE is present, else the current sweep channel from
`setCurrentChannel`, held atomically for the driver-task read — ADR-0013 decision #3). When the table
is full it sets `overflowed()` rather than dropping silently (decision #4). `count()`/`at()` expose
the enumeration; `reset()` clears it. The parse is bounded and allocation-free, so it stays inside §4
invariant #10 (ADR-0011 decision #3).

**The channel parser (pure).** `beaconChannel(frame, len)` in `src/net/eapol.{h,cpp}` walks the
tagged-parameter list after the fixed beacon body (the same walk as `beaconSsid`) for the DS
Parameter Set element (id 3, length 1) and returns its channel, or 0 if absent or malformed — an
honest "unknown", never a guess (ADR-0013 decision #3).

**The seam (device).** `RadioSniffer` gains `setChannel(channel)` (ADR-0013 decision #1);
`Esp32RadioSniffer::setChannel` is a single `esp_wifi_set_channel`, returning false if the radio
rejects the channel. The pure hopper/registry need no radio to test, so `setChannel`'s on-air
behaviour is proved by Scenario D, as `begin`/`stop` were by slice-0012.

**The bench probe (device-only, `SAPPER_TEST_HOOKS`).** `src/rf_discover_probe.{h,cpp}` — when
`SAPPER_TEST_RF_HOP` is set (a channel list), it parses the list and dwell (`SAPPER_TEST_RF_DWELL_MS`,
default 300 ms), wires `Esp32RadioSniffer → ApRegistry` and a `ChannelHopper`, begins on the first
channel, and on each pump drives `hopper.tick(millis())` — retuning via `setChannel` and reporting
`[HOP] channel=…` on a hop, and `[DISCOVER] ap …` for each newly seen AP. Empty var → inactive
(normal boot runs, mirroring empty credentials → portal); no `SAPPER_TEST_HOOKS` → no-op stubs, so no
shipped binary hops (ADR-0013 decision #7). `main.cpp` gives it first refusal at boot, before the
slice-0012 sniff probe.

**Why the dwell is a parameter, not a hardcoded number.** The reliable minimum dwell is a
hardware measurement this slice cannot run (ADR-0013 decision #5); the default (300 ms ≈ 3× the
102.4 ms beacon interval) is a spec-derived floor, and Scenario D's artifact reports per-channel
reception at the configured dwell so an operator tunes it and amends ADR-0013. The verify is the
probe.

## Verification

- **Native Unity tests** (Scenarios A–C) — `test/test_channel_hopper` drives the schedule against a
  fake clock; `test/test_ap_registry` drives beacons through `onFrame()` and asserts the enumeration,
  the DS-Param channel, the fallback, and overflow; `test/test_eapol` gains `beaconChannel` cases.
  Wired as `npm run test:native` (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1). The
  abstract seam and the one-line `setChannel` have no host logic to test; they are proved on air by
  Scenario D.
- **On-air verify** (Scenario D) — `scripts/0014-rf-discover-verify.mjs`, wired as
  `npm run verify:rf-discover` (R11). Runs on a workstation with the board attached, never in cloud CI
  (§4 invariant #5); its error-check half (fail on any `[FATAL]`/`[ERROR]`, require hops across ≥2
  channels and ≥1 discovered AP) is machine-checkable, and it writes the observed `[HOP]`/`[DISCOVER]`
  lines as the review artifact — which doubles as the dwell measurement (ADR-0013 decision #5).
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) links the shipped
  `.elf`; the new device sources (`ap_registry.cpp` shares the native lane; `rf_discover_probe.cpp`,
  and the `setChannel` override) compile under the ESP toolchain, and the shipped build carries no
  active hopper (no-op stubs), so linked behaviour and RAM/flash stay unchanged.

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each Definition-of-Done
scenario was met or recording the deviation.)_
