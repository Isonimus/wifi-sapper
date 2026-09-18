---
id: '0012'
title: "RF sniffer: the fixed-channel promiscuous RX seam feeding the pure capture core"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Build the radio→raw-bytes bridge that ADR-0009 left open and ADR-0011 specifies: a `RadioSniffer`
seam that owns promiscuous mode on one fixed channel and forwards every received 802.11 frame, as
raw bytes, into the pure capture core (slice-0010). This is the first slice that touches the radio,
and it is deliberately the thinnest honest one — the seam and its ESP-IDF implementation, wired to
the existing `HandshakeCollector` through a `HandshakeConsumer` adapter.

It does **not** hop channels (fixed channel only — hopping is a follow-on, ADR-0011), does **not**
discover APs or select targets (the collector is told its target BSSID), does **not** transmit
(no deauth), and does **not** persist or upload captures (no emit path — that is slice-5). There is
no shipped capture loop yet: the endless AutoHunt loop and the promiscuous-vs-STA radio-mode switch
belong to the HuntEngine (slice-4). Until then the device sniffer-run path is bench scaffolding
gated behind `SAPPER_TEST_HOOKS` (`rf_sniff_probe`), which exists only so the lane-3 verify has real
firmware to drive.

## Definition of Done

**Scenario A — the RX seam delivers frames into the pure core (host).**
- **Given** a `HandshakeCollector` targeting a specific BSSID, wrapped in a `HandshakeConsumer`
  (the concrete `FrameConsumer`)
- **When** that BSSID's beacon, M1, and M2 frames are delivered through `HandshakeConsumer::onFrame()`
  — the same entry point the device's promiscuous callback calls — with a frame from a *different*
  BSSID interleaved
- **Then** the collector reports `isWpaSecValid()` true, has stored the beacon's bytes unmodified
  and read its SSID, and has ignored the foreign frame; delivering only a foreign frame leaves the
  collector empty. This proves the seam's `onFrame` path reaches the core, with no radio.
- **Proof:** `npm run test:native` → `test/test_radio_sniffer`.

**Scenario B — the promiscuous seam receives real frames on the fixed channel (on-air, lane 3).**
- **Given** a Cardputer flashed with the `cardputer_testhooks` build, `SAPPER_TEST_RF_CHANNEL` and
  `SAPPER_TEST_RF_BSSID` set to a known access point on a known channel, and that AP powered nearby
- **When** `npm run verify:rf-sniffer` drives the device
- **Then** the device enters promiscuous mode on that channel (`[SNIFF] listening …`) and, within
  the timeout, the target AP's **beacon reaches the pure core** and is reported
  (`[SNIFF] beacon bssid=… ssid=…`); the run fails on any `[FATAL]`/`[ERROR]` line and writes the
  observed lines as its artifact.
- **Proof:** `scripts/0012-rf-sniffer-verify.mjs`, wired as `npm run verify:rf-sniffer`.

## Design

**The seam (pure).** `src/net/radio_sniffer.h` declares two abstract interfaces (ADR-0011
decision #1): `FrameConsumer` (`onFrame(frame, len)`) and `RadioSniffer` (`begin(channel,
consumer)` / `stop()`). Both compile on the native lane; neither carries protocol logic.
`src/net/handshake_consumer.h` is the one adapter — `HandshakeConsumer` holds a
`HandshakeCollector&` and forwards each frame into `ingest()`. It is header-only and pure, so the
host test drives the exact production wiring the device uses.

**The implementation (device-only).** `src/net/radio_sniffer_esp32.{h,cpp}` implements
`Esp32RadioSniffer` over `esp_wifi`. `begin()` brings the driver up in a station role with no
association, installs the RX callback, sets the promiscuous filter to management + data frames (a
radio-level filter, not code — ADR-0011 decision #2), enables promiscuous mode, and sets the
channel. The RX callback copies the raw frame to the registered consumer and returns — no
allocation, no blocking, no parsing in the Wi-Fi-driver task (ADR-0011 decision #3, §4 invariant
#10). The context-free ESP-IDF callback forces a single file-scope consumer pointer, cleared by
`stop()`; it holds no protocol state and is confined to this wrapper (ADR-0011).

**The bench probe (device-only, `SAPPER_TEST_HOOKS`).** `src/rf_sniff_probe.{h,cpp}` is the
scaffolding the verify script drives. When `SAPPER_TEST_RF_BSSID` is set, it parses the target,
wires `Esp32RadioSniffer → HandshakeConsumer → HandshakeCollector`, begins sniffing on
`SAPPER_TEST_RF_CHANNEL`, and reports `[SNIFF]` lines; when the var is empty it is inactive and the
normal boot runs (mirroring how empty test credentials route to the portal). In a build without
`SAPPER_TEST_HOOKS` it compiles to no-op stubs, so no shipped binary sniffs (ADR-0011 decision #5).
`main.cpp` gives it first refusal at boot and pumps it in `loop()` when active.

**Why Scenario B stops at the beacon.** wpa-sec's real prize is a 4-way handshake, but capturing
one on air needs a client to (re)join — either waited for or forced by deauth — and then an
emission path to turn the frames into an uploadable pcap. Deauth is a later slice and the emit path
is slice-5; forcing either into this seam-only slice would re-bundle what ADR-0009 split. A beacon
is emitted continuously by any AP, so beacon-reception is the deterministic, choreography-free proof
that the seam receives real frames and routes them through the real consumer into the core — exactly
what this slice adds. The full on-air handshake→pcap proof is deferred to the slice that owns the
emit path (LEDGER, ADR-0011).

## Verification

- **Native Unity test** (Scenario A) — `test/test_radio_sniffer` drives `HandshakeConsumer::onFrame()`
  (the seam entry point) and asserts frames reach the `HandshakeCollector`: a beacon+M1+M2 set makes
  the collector wpa-sec-valid with the beacon bytes intact, and a foreign frame is ignored. Wired as
  `npm run test:native` (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1). The abstract
  seam has no logic to test; the device implementation is proved on air by Scenario B.
- **On-air verify** (Scenario B) — `scripts/0012-rf-sniffer-verify.mjs`, wired as
  `npm run verify:rf-sniffer` (R11). Runs on a workstation with the board attached, never in cloud
  CI (§4 invariant #5); its error-check half (fail on any `[FATAL]`/`[ERROR]`) is machine-checkable
  and it writes the observed `[SNIFF]` lines as the review artifact.
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) links the shipped
  `.elf`. The device build compiles all of `src/`, so `radio_sniffer_esp32.cpp` and the no-op
  `rf_sniff_probe.cpp` compile under the ESP toolchain; the shipped build carries no active sniffer,
  so linked behaviour is unchanged and the build must stay green.

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each Definition-of-Done
scenario was met or recording the deviation.)_
