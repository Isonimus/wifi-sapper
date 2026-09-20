---
id: '0028'
title: "Deauth/disassoc frame builder, raw-TX seam, and bench-gated on-air proof"
type: slice
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Goal

Build the deauth mechanism ADR-0009 #4 named but deferred, and close the LEDGER's owed on-air proof
of a real deauth-forced 4-way handshake becoming a wpa-sec-valid pcap. Per ADR-0027: a pure
`buildDeauthFrame` (host-tested), a thin `RawTransmitter` seam whose device implementation carries
the `esp_wifi_80211_tx` path and the SDK sanity-check bypass, and a bench probe
(`SAPPER_TEST_HOOKS`) that transmits at a target and captures the handshake the deauth forces. The
shipped hunt loop does **not** deauth — autonomous, allowlisted deauth is the separate LEDGER slice.
The raw-TX capability is absent from any build without `SAPPER_TEST_HOOKS` (ADR-0027 decision 3).

## Definition of Done

**Scenario A — a deauth frame has the exact 802.11 byte layout (host).**
- **Given** an AP BSSID and a destination MAC
- **When** `buildDeauthFrame` is called with `ManagementSubtype::Deauth`
- **Then** it returns `kDeauthFrameLen` (26) and the buffer is: byte 0 = `0xC0`, byte 1 = 0,
  bytes 2–3 = 0, bytes 4–9 = the destination, bytes 10–15 = the BSSID (spoofed source), bytes
  16–21 = the BSSID, bytes 22–23 = 0, bytes 24–25 = the reason code little-endian.
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario B — disassoc differs from deauth only in the subtype (host).**
- **Given** the same inputs
- **When** the frame is built with `ManagementSubtype::Disassoc` instead of `Deauth`
- **Then** byte 0 is `0xA0` and every other byte is identical to the deauth frame — the two kinds
  differ only in the frame-control subtype (ADR-0027).
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario C — the reason code is written little-endian (host).**
- **Given** a named non-default reason (`DeauthReason::Class3FrameFromNonassoc`)
- **When** the frame is built
- **Then** byte 24 is the reason's low byte and byte 25 its high byte, and a distinct reason
  produces distinct bytes there — so the reason is carried, not hard-coded.
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario D — an undersized buffer is refused, not truncated (host).**
- **Given** a buffer smaller than `kDeauthFrameLen`
- **When** `buildDeauthFrame` is called
- **Then** it returns 0 and writes no partial frame — a truncated management frame is a silently
  wrong actuation (quality-bar §3, fail loud).
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario E — the destination is exactly the caller's, broadcast or unicast (host).**
- **Given** the broadcast address in one call and a specific client MAC in another
- **When** each frame is built
- **Then** Addr1 (bytes 4–9) is exactly the address passed, while the source and BSSID stay the AP —
  so broadcast (all clients) and targeted (one client) deauth are both expressible with no scanner.
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario F — the builder's bytes reach the transmitter seam unchanged (host).**
- **Given** a `FakeRawTransmitter` recording what it is handed
- **When** a built deauth frame is passed to `RawTransmitter::transmit`
- **Then** the transmitter receives exactly the bytes and length the builder produced — the
  builder→seam contract holds through the same `transmit()` the device TX uses (§4 invariant #2),
  with no building logic in the seam.
- **Proof:** `npm run test:native` → `test/test_deauth`.

**Scenario J — on-air deterministic gate: the device transmits real deauth frames (device).**
- **Given** a flashed `SAPPER_TEST_HOOKS` board with `SAPPER_TEST_DEAUTH` set to a target
  BSSID + channel
- **When** the probe runs
- **Then** the SDK sanity-check bypass reports active, and the device transmits built deauth **and**
  disassoc frames at the target repeatedly with `transmit()` succeeding and no `[FATAL]`/`[ERROR]`;
  the probe reports the count of its own well-formed deauth frames heard back on the promiscuous
  seam (radio permitting). This half needs no other station and is the pass/fail gate.
- **Proof:** `npm run verify:deauth` (`scripts/0028-deauth-verify.mjs`); artifact
  `artifacts/0028-deauth-tx.txt` (the probe log with the TX/self-heard counts).

**Scenario K — on-air opportunistic e2e: deauth forces a captured, wpa-sec-valid pcap (device).**
- **Given** the probe pointed at an **operator-authorized** AP with a connected client
- **When** the forced client reassociates and its 4-way handshake is sniffed
- **Then** the probe accumulates Beacon + M1 + M2 for the target, serializes a `LINKTYPE_IEEE802_11`
  pcap, reports it wpa-sec-valid, and streams it for the verify to save — the owed proof that deauth
  forces a real capture. Environmental (a client must reconnect), so this is **not** the gate; the
  committed pcap is the evidence when captured.
- **Proof:** `npm run verify:deauth` when a client is present; artifact `artifacts/0028-deauth-forced-handshake.pcap`.

## Design

Per ADR-0027. Components:

- **`net/deauth.{h,cpp}`** (pure, host-tested) — `ManagementSubtype`, `DeauthReason`,
  `kDeauthFrameLen`, and `buildDeauthFrame(out, cap, subtype, dest[6], bssid[6], reason)`. No radio,
  no `Serial`. The write-side analogue of `eapol` (§4 invariant #8).
- **`net/raw_transmitter.h`** — the abstract `RawTransmitter` seam (one `transmit(frame, len)`),
  device-agnostic, compiles native.
- **`net/raw_transmitter_esp32.{h,cpp}`** (device-only, `#if defined(SAPPER_TEST_HOOKS)`) —
  `Esp32RawTransmitter : RawTransmitter` wrapping `esp_wifi_80211_tx(WIFI_IF_STA, …)` plus the
  `ieee80211_raw_frame_sanity_check` weak-symbol override. Absent from shipped builds (ADR-0027 #3).
- **`deauth_probe.{h,cpp}`** + `main.cpp` dispatch — bench probe reading `SAPPER_TEST_DEAUTH`
  (BSSID + channel, optional client MAC). Brings up the `Esp32RadioSniffer` on the target channel
  with a `HandshakeCollector` aimed at the AP, transmits deauth+disassoc on a cadence, counts its
  self-heard frames, and on `isWpaSecValid()` serializes a pcap (via `serializeHandshake` into a
  buffer-backed `CaptureSink`) to dump. Pumps `g_channel` in its `loop()` branch (the slice-0026
  lesson) so `dump`/observation commands are answered.
- **`test/support/fake_raw_transmitter.h`** — records the last transmitted frame + a call count.
- **`scripts/0028-deauth-verify.mjs`** (`npm run verify:deauth`) — drives `cardputer_testhooks`
  with `SAPPER_TEST_DEAUTH`; asserts the bypass is active and TX succeeds with no `[FATAL]`/
  `[ERROR]` (the gate); saves the TX-count log and, when a forced handshake is captured, reconstructs
  the pcap artifact.

## Verification

- **Scenarios A–F** (pure): `test/test_deauth/`, native Unity lane (ADR-0004 lane 1),
  `npm run test:native`, against a `FakeRawTransmitter`.
- **Scenario J** (on-air deterministic gate) and **K** (on-air opportunistic e2e):
  `scripts/0028-deauth-verify.mjs` (`npm run verify:deauth`), driving `env:cardputer_testhooks`
  with `SAPPER_TEST_DEAUTH`; fails on any `[FATAL]`/`[ERROR]`; artifacts
  `artifacts/0028-deauth-tx.txt` and (when captured) `artifacts/0028-deauth-forced-handshake.pcap`.
- Board compile (ADR-0004 lane 2): `pio run -e cardputer` and `-e cardputer_testhooks`.

## As built

Shipped as designed (ADR-0027), with these deviations and additions worth recording:

- **The mechanism shipped, autonomous deauth did not** — per the scope decision. The pure
  `buildDeauthFrame`, the `RawTransmitter` seam, its `SAPPER_TEST_HOOKS`-gated device implementation,
  and the bench probe landed; the `HuntEngine` was left untouched. During this slice the operator
  reframed the appliance's intended use to **autonomous deauth of every network in the loop** (an
  authorized industrial-pentest factory), which is the next slice — recorded in the LEDGER, and its
  ADR will *supersede* ADR-0027's `SAPPER_TEST_HOOKS` gating to ship raw-TX behind a provisioning
  toggle (default-on). This slice stayed the proven foundation for it.
- **Measured cost:** both board envs compile; shipped `cardputer` Flash 40.6%, RAM 35.3%. The
  `cardputer_testhooks` image is only **~548 bytes larger** than the shipped one — that delta *is* the
  raw-TX capability (the `Esp32RawTransmitter`, the `esp_wifi_80211_tx` path, the SDK bypass, the
  probe); in the shipped build it is absent, and the weak-symbol override linked with no
  "multiple definition" error. The blind reviewer independently traced the four `#if` sites and
  confirmed invariant #15's gating intact.
- **Host lane: 6 focused tests** (`test_deauth`, Scenarios A–F), run via direct g++ (`pio` native is
  broken here — UnknownPlatform — the established workaround). The reviewer reproduced them.
- **On-air Scenario J PASSED** against the flashed board, re-run on the *final* binary after the
  review fixes: the device transmitted deauth **and** disassoc frames continuously with
  **`txOk` climbing to 496, `txFail=0`** over the run (artifact `artifacts/0028-deauth-tx.txt`) —
  proof the SDK sanity-check bypass is active and `esp_wifi_80211_tx` accepts our raw management
  frames on this ESP32-S3.
- **`selfHeard=0` throughout — a recorded hardware datum.** This ESP32-S3 does not loop its own TX
  back through the promiscuous RX path, so the self-heard count is *informational*, never part of the
  pass/fail gate (the gate is `txOk` climbing with `txFail=0`, which proves the radio accepted each
  raw frame). `esp_wifi_80211_tx == ESP_OK` proves the driver queued the frame; the definitive
  "on-air, well-formed" confirmation would need a second receiver, which Scenario K supplies when a
  client is present.
- **Scenario K did not trigger this run** (`captured=0`): no client reassociated during the window.
  The path is built and reviewed and emits a wpa-sec-valid pcap (`artifacts/0028-deauth-forced-handshake.pcap`)
  when a client reconnects; it is opportunistic and environmental, never the gate (ADR-0027 #5).
- **A real firmware/verify bug the first on-air run caught** (like slice-0026's serial-pump): the
  verify originally gated on the boot-once `[DEAUTH] target …` line, but the S3 USB-CDC does not
  reliably reset on port-open, so a device that had already booted was missed. Fixed by gating on the
  *repeating* `[DEAUTH] txOk=…` heartbeat. Its fail-before/pass-after evidence is Scenario J itself
  (timed out with the old gate, passed with the new).
- **Adversarial review (blind, Sonnet 5) — 3 correctness findings, all fixed, none rejected;** the
  frame byte layout and the invariant-#15 gating were independently confirmed clean.
  1. The `begin()` bypass self-check was a tautology (it could only call our own strong override,
     which always returns 1) and its `[FATAL]` branch was dead — **removed**; an ineffective bypass is
     revealed instead by `txFail>0 / txOk=0`, which the verify gates on.
  2. Serializing the forced-handshake pcap on the app task could tear a frame the driver task was
     mid-`memcpy` writing (a re-forced handshake), yielding a silently corrupt pcap — **fixed** by
     quiescing the sniffer (detach the consumer + a settle) before serializing and stopping TX after
     capture, matching invariant #11's pattern. Device-only concurrency (no host lane), like the
     probe itself.
  3. The probe's header comment overclaimed `selfHeard` as part of a "deterministic, no other
     station" gate; **corrected** to informational (see the datum above) — not fixed by gating on it,
     which would wrongly fail on hardware that does not loop TX back.
  Plus a redundant `out[flags]=0` write after the `memset` — removed.
- **Deferrals recorded** (LEDGER): the `parseBssid` parser is duplicated across `rf_sniff_probe` and
  `deauth_probe` (second instance, not hoisted — rule of three); and autonomous deauth in the hunt
  loop (the reframed operating mode, provisioning toggle default-on, supersedes ADR-0027's gating).
