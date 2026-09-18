---
id: '0009'
title: "Capture engine as a pure protocol core behind HAL seams, emitting a wpa-sec pcap"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

The Sapper's reason to exist is the capture→upload→sync spine: sniff WPA/WPA2 4-way
handshakes, write each as a pcap, upload to `wpa-sec.stanev.org`, sync cracked results. The
`LEDGER.md` roadmap collapses the first half into one line — "slice-3: capture engine — port
handshake_capture + deauth + station_scanner + pcap writer (headless)". That is four
subsystems, not one work-unit. This ADR settles *how* the capture side is built so those
subsystems can land as separate slices without each re-deciding its own architecture.

The parent Adversary implements all four (`../adversary/src/modules/capture/handshake_capture.{h,cpp}`,
`../adversary/src/modules/attack/deauth.{h,cpp}`, `../adversary/src/modules/wifi/station_scanner.h`,
`../adversary/src/modules/pcap/pcap_writer.{h,cpp}`), and it is a working reference for the
*protocol* — but not for the *shape*. Three of its choices collide with this repo's doctrine:

- **Singletons with hidden static callbacks.** `HandshakeCapture::getInstance()` owns a static
  `promiscuousCallback` that mutates instance state as a side effect of ESP-IDF's promiscuous
  hook. That is precisely the untestable, hardware-entangled structure ADR-0004 (native lane 1)
  and ADR-0008 (fake-`Preferences` seam) exist to avoid. Parsing an EAPOL frame has no reason to
  touch hardware, yet in the reference it can only run on a device.
- **The pcap writer is welded to `SD.h`.** `pcap_writer.cpp` calls `SDManager::getInstance()`
  and `SD.open()` directly, so the serialization (a pure byte layout) cannot be produced or
  checked without a card. ADR-0001 made SD optional; a capture path that assumes it fails that
  contract.
- **A latent format bug.** The reference declares the pcap global header `network = 105`
  (`LINKTYPE_IEEE802_11`, raw frames) yet writes every record through
  `writePacketWithRadiotap()`, which prepends an 8-byte radiotap header whose `it_present = 0`.
  The declared link type therefore does not match the record content, and the empty radiotap
  carries no fields anyway — 8 bytes of nothing in front of a frame the reader is told is raw.

What the reference *does* settle, with values worth citing so this decision rests on data:

- EAPOL rides the frame after an LLC/SNAP header `AA AA 03 00 00 00 88 8E`, found by scanning
  from offset 24 (the 802.11 header is 24–30 bytes).
- The handshake message (M1–M4) is read from the EAPOL-Key **Key Information** field
  (2 bytes, big-endian, at EAPOL offset 5): M1 = Pairwise+Ack; M2 = Pairwise+MIC; M3 =
  Pairwise+Ack+MIC+Install+Secure; M4 = Pairwise+MIC+Secure.
- The BSSID's position depends on the To/From-DS bits (Addr1 / Addr2 / Addr3).
- **The minimum a wpa-sec upload needs is Beacon + M1 + M2** (`CapturedHandshake::isWpaSecValid()`),
  and the pcap records are the **full 802.11 frames**, not the EAPOL payloads.

## Decision

**1. The capture engine is a pure protocol core fed through HAL seams — no singletons.** All
802.11/EAPOL interpretation and all pcap serialization are ordinary C++ compiled and unit-tested
on the native lane (ADR-0004 lane 1), in the `sapper` namespace, with no `esp_wifi`, `WiFi`,
`SD`, or `Serial` dependency. Hardware enters only through two thin seams, and neither contains
protocol logic:

- an **RX seam** — the ESP-IDF promiscuous callback (a later RF slice) does nothing but forward
  raw frame bytes into the core;
- a **`CaptureSink` seam** — the core writes serialized pcap bytes into an abstract sink; the
  device implementation (SD / LittleFS / an in-RAM upload buffer) is chosen later, and a host
  implementation is a plain buffer.

This is the same cut slice-0005 and slice-0007 used (pure core tested natively, hardware proved
by a verify script) and the same seam doctrine as ADR-0008, applied to capture.

**2. The pcap format is classic pcap, link type 105, raw 802.11 frames — no radiotap.** Global
header: magic `0xa1b2c3d4`, version 2.4, `thiszone = 0`, `sigfigs = 0`, `snaplen = 65535`,
`network = 105` (`LINKTYPE_IEEE802_11`). Each record is a 16-byte header (`ts_sec`, `ts_usec`,
`incl_len`, `orig_len`) followed by the **full 802.11 frame**. Radiotap is deliberately dropped:
the reference's radiotap was empty (`it_present = 0`) and so carried no channel/RSSI/rate a
consumer could use, while making the declared link type a lie about the record content. Omitting
it makes the declared link type match what is actually stored — fail-loud consistency
(quality-bar §3) — and `hcxpcapngtool`/wpa-sec accept `LINKTYPE_IEEE802_11` directly. If a real
radiotap with populated fields is ever wanted, that is a new decision with `network = 127`, not
an empty header under the wrong link type.

**3. A handshake is uploadable at Beacon + M1 + M2; that is the core's completion test.** The
core accumulates, for a single target BSSID, the beacon and whichever of M1–M4 it sees, storing
each as a full frame. `isWpaSecValid()` (Beacon + M1 + M2) is the gate for emitting a pcap;
`isComplete()` (all four) is reported but not required. The core does **not** parse ANonce,
SNonce, or MIC into fields the way the reference does — `hcxpcapngtool` recovers those from the
frames, so extracting them on-device is dead work. The core keeps only what serialization and
targeting need: the raw frames, the BSSID, the SSID (from the beacon), and the channel.

**4. Deauth, the station scanner, and the RF sniffer are their own slices, built to this shape.**
Each splits the same way: the pure half (deauth/disassoc frame *building*; station-table
*accounting*; frame *parsing*) is host-tested; the hardware half (raw TX; the promiscuous RX
seam) is a thin device wrapper proved by a lane-3 verify script. This ADR is the contract they
share; they do not each re-litigate singletons-vs-seams.

**5. PMKID (clientless) capture is out of scope.** The reference extracts a PMKID from M1's RSN
IE for a clientless attack. The Sapper's endless-autohunt loop is built on the 4-way handshake;
PMKID is a second capture mode with its own parsing and its own wpa-sec path. It is deferred to
the ledger rather than folded in speculatively (quality-bar §3, rule of three).

## Consequences

- This ADR adds two standing invariants to CLAUDE.md §4 in the same commit that accepts it:
  (a) all 802.11/EAPOL interpretation lives in the pure core, compiled and unit-tested on the
  native lane, and the RF seam only forwards raw bytes; (b) capture artifacts are emitted only
  through the `CaptureSink` seam — no module opens or writes a capture file directly (the capture
  analogue of invariant #6 for provisioning). Both are review-only: the linter cannot see "no
  logic in the seam", so `/wrap-up` is their enforcement, while the parser's presence on the
  green native lane is itself the evidence it is pure.
- The capture core produces bytes, not files. Where those bytes live between capture and upload
  (SD, LittleFS, or an in-RAM queue) is left to slice-5, when the upload/retry queue's durability
  needs actually decide it. The core stays medium-agnostic and fully host-testable in the
  meantime; this is why the storage medium is *not* chosen here.
- Splitting the roadmap's "slice-3" into capture-core → RF sniffer → deauth → station-scanner
  means the first slice ships with everything green on the native lane and nothing waiting on RF
  hardware. The cost is more slices; the benefit is that each has a reviewable surface and its own
  Definition of Done, per the one-work-unit rule (CLAUDE.md §1).
- Promiscuous capture and STA association are mutually exclusive radio modes. The core does not
  care, but the engine that drives it (slice-4 HuntEngine) must own the switch between sniffing
  and the connected state an upload needs. Recorded here so that later work does not mistake the
  core's silence on the matter for a claim that they compose freely.
- Not carrying PMKID means a clientless-but-present AP with no associating clients yields no
  capture until a client appears (deauth accelerates that). Accepted for now; the ledger holds
  the PMKID option so the omission is visible, not implicit.
