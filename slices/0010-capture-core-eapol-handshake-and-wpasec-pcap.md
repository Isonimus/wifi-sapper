---
id: '0010'
title: "Capture core: EAPOL handshake parsing, accumulation, and wpa-sec pcap serialization"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Build the pure heart of the capture engine: given raw 802.11 frames (as a promiscuous sniffer
would later hand them over), identify the beacon and the WPA/WPA2 4-way-handshake messages for a
target BSSID, accumulate them, and serialize a wpa-sec-uploadable **pcap** into a `CaptureSink`.
This is the concrete first implementation of ADR-0009: everything here is host-testable C++ with
no radio, no filesystem, and no `Serial` — the RF sniffer that feeds it, the raw-TX deauth that
accelerates it, and the storage medium that persists the bytes are each their own later slice.

This refines the `LEDGER.md` `slice-3` roadmap line, which bundled four subsystems into one; per
ADR-0009 it is cut into capture-core (this slice) → RF sniffer → deauth → station-scanner. It
does **not** capture anything over the air (no radio yet), does **not** choose a storage medium
(the sink is abstract; the device implementation is slice-5's), and does **not** handle PMKID
(deferred, ADR-0009). It carries the protocol constants and message-identification logic forward
from the Adversary's `handshake_capture`, rebuilt as a pure core rather than a singleton.

## Definition of Done

**Scenario A — EAPOL/802.11 frame interpretation is unit-tested.**
- **Given** the `native` env, with no board attached
- **When** `npm run test:native` runs
- **Then** the pure parser: identifies M1/M2/M3/M4 from the EAPOL-Key **Key Information** bits
  (Pairwise+Ack → M1; Pairwise+MIC → M2; Pairwise+Ack+MIC+Install+Secure → M3; Pairwise+MIC+Secure
  → M4) and returns `Unknown` for any other flag combination, a non-Key EAPOL type, or a frame too
  short to be an EAPOL-Key; locates the EAPOL payload only when the LLC/SNAP header
  `AA AA 03 00 00 00 88 8E` is present; extracts the BSSID from the correct address field for each
  To/From-DS combination; and reads a beacon's SSID from its IE list (including the hidden-SSID
  empty case).

**Scenario B — the accumulator collects a wpa-sec-valid handshake for one target and rejects noise.**
- **Given** a `HandshakeCollector` targeting a specific BSSID
- **When** it ingests, in any order, that BSSID's beacon, M1, and M2 frames (plus frames from a
  *different* BSSID interleaved)
- **Then** it reports `isWpaSecValid()` true, stores the full 802.11 bytes of each of the three
  frames unmodified, records the SSID from the beacon, and has ignored every foreign-BSSID frame;
  and **given** only M1 (no beacon, no M2) it reports `isWpaSecValid()` false, while a full M1–M4
  set additionally reports `isComplete()` true. `reset()` returns it to the empty state.

**Scenario C — a collected handshake serializes to a well-formed wpa-sec pcap.**
- **Given** a `HandshakeCollector` holding a wpa-sec-valid handshake and an in-memory `CaptureSink`
- **When** `serializeHandshake()` writes to the sink
- **Then** the sink holds a classic pcap: global header magic `0xa1b2c3d4`, version 2.4,
  `snaplen 65535`, `network 105` (`LINKTYPE_IEEE802_11`, no radiotap), followed by exactly the
  Beacon, M1, and M2 records (and M3/M4 when present) in that order, each record's `incl_len` and
  `orig_len` equal to its stored frame length and its bytes identical to what was ingested;
  serialization of a non-wpa-sec-valid handshake returns false and writes nothing, and a sink that
  fails a write causes `serializeHandshake()` to return false rather than silently truncating
  (quality-bar §3, fail loud).

## Design

**Three pure modules, one per responsibility (ADR-0009 decision #1).**

- `src/net/eapol.{h,cpp}` — *read one frame*. Free functions in `sapper`: `frameSubtype()`,
  `frameBssid()` (To/From-DS → Addr1/Addr2/Addr3), `locateEapol()` (scan offsets 24–39 for the
  LLC/SNAP `AA AA 03 00 00 00 88 8E`, return the EAPOL slice), `parseKeyInfo()` and
  `identifyMessage()` (the Key-Information bit logic), and `beaconSsid()` (walk the beacon IE list
  to the SSID element). Types `HandshakeMessage` and `EapolKeyInfo`. These are the constants and
  logic carried from `../adversary/src/modules/capture/handshake_capture.cpp`, rebuilt as free
  functions so a test calls them directly with no instance.
- `src/net/handshake_collector.{h,cpp}` — *accumulate the set*. `CapturedHandshake` (BSSID, SSID,
  channel, and full-frame buffers for beacon + M1–M4) and `HandshakeCollector`, which `ingest()`s
  raw frames, matches the target BSSID, classifies each via `eapol.h`, stores the full frame, and
  answers `hasBeacon()` / `has(msg)` / `isWpaSecValid()` / `isComplete()` / `reset()`. It stores
  **full 802.11 frames**, not EAPOL payloads (ADR-0009 decision #2), and does **not** parse nonces
  or MIC — `hcxpcapngtool` recovers those, so extracting them here would be dead work (ADR-0009
  decision #3).
- `src/net/pcap.{h,cpp}` — *emit the bytes*. The `CaptureSink` abstract seam (`write(data,len) →
  bool`, false on any short/failed write) and `serializeHandshake(const CapturedHandshake&,
  CaptureSink&)`, which writes the global header then the Beacon/M1/M2(/M3/M4) records. It refuses
  a handshake that is not `isWpaSecValid()` (returns false, writes nothing) and propagates any sink
  failure (returns false) rather than emitting a truncated file.

**Frame buffer bound.** Each stored frame is capped at 512 bytes, matching the reference's
largest buffer (M3 with encrypted key data, and its beacon cap). A frame longer than the cap is
rejected at ingest rather than truncated — a truncated frame in a pcap is a silently corrupt
capture, which fail-loud forbids.

**Timestamps.** wpa-sec matches on frame content and the Beacon→M1→M2 ordering, not wall-clock, so
`serializeHandshake()` assigns each record a synthetic increasing timestamp (the emission index)
rather than depending on a device clock the pure core does not have. The device clock is
irrelevant to the crack; the comment at the site says so, so a later operator does not "fix" it by
threading `millis()` through the pure layer.

**Why no `## Verification` verify script.** Every claim this slice makes is assertable by a unit
test — there is no rendering, timing, or RF behaviour that needs a device (project CLAUDE.md §3).
The on-air proof (a real handshake captured to a real pcap that wpa-sec accepts) belongs to the RF
sniffer slice that adds the promiscuous seam feeding this core; it is named there, against this
core, not fabricated here.

## Verification

- **Native Unity tests** (Scenarios A, B, C) — `test/test_eapol` (frame/EAPOL parsing and
  message identification, every branch and rejection), `test/test_handshake_collector` (target
  matching, wpa-sec/complete gating, foreign-BSSID rejection, reset), and `test/test_pcap`
  (global-header fields, record count and order, per-record `incl_len`/`orig_len` and byte
  fidelity, the not-valid and failing-sink refusals). Wired as `npm run test:native`
  (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1). This slice is pure, so the native
  lane is its whole proof — no `scripts/*-verify.mjs`, because there is no runtime behaviour a unit
  test cannot assert (project CLAUDE.md §3); the RF slice that feeds real frames into this core
  carries the lane-3 on-air proof.
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) still links the
  shipped `.elf`. The device build compiles all of `src/`, so the new pure sources compile under the
  ESP toolchain too — a real portability check — but nothing references them yet, so the linked
  behaviour is unchanged and the build must stay green.

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each Definition-of-Done
scenario was met or recording the deviation.)_
