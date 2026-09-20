---
id: '0027'
title: "Deauth actuation: a pure frame builder, a raw-TX seam, and a bench-gated on-air proof"
type: architecture
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Context

ADR-0009 decision #4 named deauth as its own slice — *"the pure half (deauth/disassoc frame
building) is host-tested; the hardware half (raw TX) is a thin device wrapper proved by a lane-3
verify"* — but did not build it. It is the last unbuilt piece of the capture spine, and the LEDGER
owes two things against it: the deauth **mechanism** (pure frame building + a raw-TX seam), and the
**on-air proof** of a real deauth-forced 4-way handshake becoming a wpa-sec-valid pcap — the one
capture claim slice-0018 could only prove with a *synthetic* stimulus, because nothing yet forces a
real handshake on air.

Deauth is the appliance's one deliberately dangerous capability, and two recorded decisions pull in
opposite directions about it:

- ADR-0003's context states plainly that *"the Sapper's entire purpose is an autonomous transmit
  path (deauth)"* — so autonomous deauth is the intended end state, not a forbidden one.
- But §4 invariant #4 forbids any build without `SAPPER_TEST_HOOKS` from being *commanded to deauth
  over serial*, and the LEDGER carves out a **separate** future slice — "allowlist-only operating
  mode for unattended deauth" — as the safety rail that must exist before deauth runs unattended in
  the field.

The resolution is a split by work-unit (CLAUDE.md §1), the same discipline ADR-0009 #4 already
applied to the roadmap's monolithic "slice-3": **this** slice builds and proves the mechanism
behind the bench gate; the **allowlist** slice later turns it on autonomously behind a runtime
safety check. Bundling them would ship an unrestricted weaponizable capability ahead of its safety
rail.

The parent Adversary's `../adversary/src/modules/attack/deauth.cpp` is the working reference for the
*protocol*, and settles the values this decision rests on:

- A deauth/disassoc is a fixed **26-byte** 802.11 management frame. Byte 0 is the frame-control
  subtype — **`0xC0` deauthentication, `0xA0` disassociation** — and is the *only* structural
  difference between the two. Byte 1 (FC flags) is 0; bytes 2–3 (duration) are 0; bytes 4–9 are
  Addr1 (destination), 10–15 Addr2 (source), 16–21 Addr3 (BSSID); bytes 22–23 (sequence) are 0; and
  bytes **24–25 carry the reason code little-endian**. To knock a client off its AP the frame is
  spoofed *from the AP*, so Addr2 (source) and Addr3 (BSSID) are both the AP's BSSID and Addr1 is
  the client (or the broadcast address to reach every client at once).
- ESP-IDF's SDK **blocks** raw management-frame TX. The pioarduino platform (Bruce's patched libs,
  named in CLAUDE.md as *"required for raw deauth TX"*) exposes `ieee80211_raw_frame_sanity_check`
  as a **weak symbol**; overriding it to return 1 for the magic argument `31337` lets the frame
  through. This override *is* the raw-TX capability — it is precisely what must be absent from a
  shipped binary.
- Raw TX (`esp_wifi_80211_tx`) works while the radio is in promiscuous mode on a channel, so a
  single bench probe can transmit a deauth and **hear it back** on the same channel through the
  existing `RadioSniffer` seam.

Two structural choices in the reference are the anti-patterns to *not* inherit — the same ones
ADR-0009 and ADR-0011 already rejected: a `getInstance()` singleton that owns WiFi-mode juggling and
mutates state as a side effect of hardware, and frame-building welded to `#ifdef ESP32`. Frame
construction has no reason to touch a radio; it is a pure byte layout, testable with no instance.

## Decision

**1. Deauth/disassoc frame *construction* is a pure builder, host-tested on the native lane.**
`net/deauth.{h,cpp}` — no `esp_wifi`, no `WiFi`, no `Serial` — exposes
`buildDeauthFrame(out, cap, subtype, dest[6], bssid[6], reason)` returning the fixed frame length
(`kDeauthFrameLen = 26`) or 0 when `cap` is too small (refused, never truncated — a short actuation
frame is a silently wrong one, quality-bar §3). `ManagementSubtype` (`Deauth`/`Disassoc`) and named
`DeauthReason` constants replace the magic subtype and reason bytes. The source address is the
BSSID (the frame is spoofed from the AP); the destination is the caller's — a client MAC or the
broadcast address. This is the write-side analogue of the pure `eapol` reader that §4 invariant #8
already governs on the read side.

**2. Raw TX is a thin `RawTransmitter` seam that forwards fully-formed bytes only.**
`net/raw_transmitter.h` is a pure abstract interface — one `transmit(frame, len) -> bool` — that
compiles on the native lane. The device implementation `net/raw_transmitter_esp32.{h,cpp}` wraps
`esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false)` and carries the
`ieee80211_raw_frame_sanity_check` weak-symbol override. It contains **no** frame-building logic —
it transmits the bytes the pure builder produced, mirroring how `RadioSniffer` forwards raw bytes
into the core without interpreting them. A host test drives the builder into a `FakeRawTransmitter`
that records the emitted frame, proving the builder→seam contract through the same `transmit()` a
real TX uses (§4 invariant #2). Unlike the RX seam, TX takes no context-free callback, so it needs
none of ADR-0011's file-scope-consumer machinery.

**3. The raw-TX *capability* is absent from any build without `SAPPER_TEST_HOOKS`.** The device TX
implementation and its SDK sanity-check bypass compile only inside `#if defined(SAPPER_TEST_HOOKS)`;
a non-hooks (shipped) build contains an empty translation unit and cannot transmit a raw frame at
all. The only caller in this slice is the bench probe, itself gated. This **strengthens** §4
invariant #4 from "cannot be *commanded* to deauth over serial" to "the deauth-transmit capability
is *not in the binary*." When the LEDGER's allowlist-gated unattended-deauth operating mode is
built, its ADR will **supersede this decision's gating** — compiling raw TX into shipped builds
guarded by the allowlist runtime check — and must state why loosening the gate is safe *then*. It is
recorded this way now so the gate is a deliberate, revisitable choice, not a silent permanent claim.

**4. Scope is the mechanism plus the on-air proof; autonomous deauth is out.** The `HuntEngine` is
untouched — it never constructs or calls the transmitter, so the shipped hunt loop does not deauth.
Forcing handshakes autonomously, and safely (allowlisted), is the separate LEDGER slice built atop
this builder and seam. This keeps a weaponizable capability behind the bench gate until its safety
rail exists, and keeps this slice to one work-unit (ADR-0009 #4).

**5. The proof splits by determinism (ADR-0004 §3).** The builder's exact byte layout — subtype
per kind, address order, little-endian reason, buffer-too-small refusal, broadcast vs unicast
destination — is unit-tested on the host, where a byte comparison is deterministic. The on-air
verify (bench, `SAPPER_TEST_HOOKS`) has two halves:

- a **deterministic gate**, which always runs and is the pass/fail: the SDK bypass is active, and
  the device accepts and transmits built deauth *and* disassoc frames at a target repeatedly with
  no `[ERROR]`/`[FATAL]`, and — radio permitting — hears its own well-formed frames back on the
  promiscuous seam (a machine check needing no other station);
- an **opportunistic end-to-end** half, which is *not* the gate: pointed at an operator-authorized
  AP with a connected client, the deauth-forced 4-way handshake is captured through the existing
  core and serialized to a wpa-sec-valid pcap, committed as the owed evidence. A client actually
  reconnecting during a run is environmental, so making it the gate would leave the slice unclosable
  on a quiet lab; the deterministic TX gate closes it, and the e2e pcap discharges the LEDGER debt
  when the conditions are present.

## Consequences

- CLAUDE.md §4 gains **invariant #15** in the same commit that accepts this ADR: deauth/disassoc
  frame *construction* lives in the pure `deauth` core on the native lane; the `RawTransmitter` seam
  forwards fully-formed frame bytes only, with no building logic; and the raw-TX capability (the
  device `esp_wifi_80211_tx` path and the `ieee80211_raw_frame_sanity_check` bypass) is absent from
  any build without `SAPPER_TEST_HOOKS` — no shipped binary transmits a deauth frame — until a
  superseding ADR loosens the gate for the allowlist operating mode. The purity and raw-bytes-seam
  halves are review-only (like #8/#9); the gating half is visible in the source (the bypass and TX
  sit inside an `#if defined(SAPPER_TEST_HOOKS)`) but is recorded review-only because the linter does
  not read §4.
- The owed LEDGER proof — a real deauth-forced handshake → wpa-sec-valid pcap — is discharged as a
  committed artifact when the e2e conditions are present on the verify run; the deterministic TX gate
  keeps the slice closable regardless. The upload of such a pcap to wpa-sec is the appliance's normal
  job, already proven in slice-0018, so this slice re-proves only the *forcing and capture*, not the
  upload.
- Targeted per-client deauth needs the passive station scanner (a separate LEDGER item). This slice
  deauths the broadcast address (every client at once) plus an optional operator-supplied client
  MAC, so it needs no scanner and does not pull that item forward.
- The allowlist unattended-deauth slice now has its contract: it consumes this builder and seam, and
  its ADR supersedes decision 3's gating to ship raw TX behind an allowlist runtime check.
