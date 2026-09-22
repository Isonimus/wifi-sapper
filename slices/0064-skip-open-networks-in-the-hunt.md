---
id: '0064'
title: "Skip open networks in the hunt"
type: slice
status: accepted
date: 2026-09-22
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0063: the hunt never targets an open (unencrypted) network, because it has no WPA 4-way
handshake to capture. Detection is by the beacon Capability Information Privacy bit — the one bit that
tells open from encrypted for every WPA generation, so a WPA1/2/3 network is never mis-classified — and
the skip is always on (an intrinsic beacon property, unlike the opt-in cracked filters of slice-0060/62).

- **`eapol`:** a new pure reader `bool beaconIsOpen(const uint8_t* frame, uint16_t len)` — true only when
  a readable capability field has the Privacy bit (0x10) clear; a frame too short to hold the field
  returns false (undecidable → captured, never skipped). Assumes a beacon the caller has classified,
  like `beaconChannel`/`beaconSsid`.
- **`ApRegistry`:** `DiscoveredAp` gains `bool isOpen = false`, set from `beaconIsOpen(frame, len)` in the
  fully-written entry before the `count_` release-store (§4 #10).
- **`HuntEngine`:** in `doStartCapturing`, before `retuneTo`, if `registry_.at(targetIndex_).isOpen` then
  `++targetIndex_; continue;` — the same skip the channel-rejected and gate-B cracked cases use. Covers
  the round-robin (which funnels through the same loop); all-open falls through to `doEnterDiscovering`.
- **Test builder:** `buildBeacon` gains `bool encrypted = true`; the default flips to encrypted (Privacy
  set) so capture-expecting tests are unchanged, and open is an explicit `encrypted=false`.
- **§4:** add invariant #29 (open-network targeting skip) in the same commit as ADR-0063.

## Definition of Done

**Scenario A — an open AP is never targeted; an encrypted one is.**
- **Given** discovery enumerates AP X (open, Privacy bit clear) and AP Y (encrypted), always on
- **When** the engine runs its discover→capture cycle and both APs' EAPOL is available
- **Then** the engine captures Y (raises `onCaptureReady` for Y) and **never enters Capturing for X**
  (X is never `capturingBssid()`, so it is also never tuned to or deauthed) — the round-robin skips X.
- **Proof:** `test/test_hunt_engine` — fails before (X is captured too), passes after. See `## As built`.

**Scenario B — all discovered APs open degrades to re-discovery, never a spin.**
- **Given** discovery enumerates only open APs
- **When** the post-discovery capture phase is reached
- **Then** the engine captures nothing and returns to Discovering (a fresh sweep) — the existing "no
  capturable target remained" fall-through, never a stall or busy-loop.
- **Proof:** `test/test_hunt_engine` — the engine re-enters Discovering with no capture reported. See
  `## As built`.

**Scenario C — `beaconIsOpen` reads the Privacy bit, and `ApRegistry` records it.**
- **Given** an open beacon (Privacy clear), an encrypted beacon (Privacy set), and a frame too short to
  hold the capability field
- **When** `beaconIsOpen` reads each, and `ApRegistry::onFrame` records an open and an encrypted beacon
- **Then** `beaconIsOpen` is true only for the open beacon (false for encrypted and for too-short), and
  the recorded `DiscoveredAp::isOpen` matches the beacon.
- **Proof:** `test/test_eapol` (`beaconIsOpen`) + `test/test_ap_registry` (`isOpen` recorded). See
  `## As built`.

## Verification

- **Target-selection skip + detection (Scenarios A, B, C)** — `pio test -e native` (`test_hunt_engine`,
  `test_eapol`, `test_ap_registry`), fail-before/pass-after recorded in `## As built`. No verify script:
  the behaviour is fully host-assertable (a fake sniffer feeding a real `ApRegistry` through the real
  `onFrame` seam, plus the pure reader unit-tested directly), like the cracked filters — unlike an
  on-air proof.

## As built

Shipped exactly as designed. `eapol` gained the pure reader `bool beaconIsOpen(const uint8_t*, uint16_t)`
reading the Capability Information Privacy bit (`kCapabilityOffset = 34`, `kCapabilityPrivacyBit = 0x10`;
`src/net/eapol.{h,cpp}`); `DiscoveredAp` gained `bool isOpen = false` set from it in the fully-written
entry before the `count_` release-store (`src/net/ap_registry.{h,cpp}`); and `HuntEngine::doStartCapturing`
skips an `isOpen` target with `++targetIndex_; continue;` before `retuneTo`, ahead of the gate-B cracked
check (`src/net/hunt_engine.cpp`). The skip is always on — no wiring, no `hunt_loop` change. `buildBeacon`
gained `bool encrypted = true` (sets `f[34] |= 0x10`); the default flip made every capture-expecting test
encrypted, so none needed editing, and the two other suites that build beacons assert nothing about the
capability byte (verified by grep before the flip).

- **Scenarios A, B — `test/test_hunt_engine`** (`test_open_ap_is_never_targeted_but_encrypted_one_is`,
  `test_all_open_degrades_to_rediscovery_not_a_spin`), driven through the real state machine via the fake
  sniffer feeding a real `ApRegistry` through the real `onFrame` seam.
- **Scenario C — `test/test_eapol`** (`test_beacon_is_open_reads_the_privacy_bit`,
  `test_beacon_is_open_too_short_frame_is_not_open`) + **`test/test_ap_registry`**
  (`test_records_open_flag_from_privacy_bit`).
- **Fail-before/pass-after:** neutralising the `doStartCapturing` skip (`if (false && …)`) failed A
  (`Expected TRUE Was FALSE` — the open AP was captured) and B (`Expected 1 Was 2` — nothing skipped, so
  the registry was not re-swept), while the deauth/round-robin/snapshot/gate-B cases stayed green.
  Neutralising `beaconIsOpen` (`return false`) failed the eapol privacy-bit test and the ap_registry
  open-flag test. Restored → all 22 `test_hunt_engine`, 18 `test_eapol`, 10 `test_ap_registry` cases and
  the full 287-case native suite pass; `cardputer` firmware builds; lint 0/0; INDEX byte-identical to the
  generator (64 documents).
- **Adversarial pass (Sonnet 5, blind to intent, aware of law — diff + CLAUDE.md + INDEX.md, read the
  files itself, no recursion):** no real defects. It independently confirmed the capability offset (34 =
  MAC header 24 + timestamp 8 + interval 2, the Privacy bit in the low octet), that `len < 35` correctly
  guards the read of index 34 and `frame == nullptr` is guarded, that no WPA generation can be
  mis-classified open (Privacy set for WEP/WPA1/2/3, clear only for open) and a too-short frame fails
  toward capturing; that `slot.isOpen` is written before the `count_` release-store (§4 #10) and read only
  on the app task in `doStartCapturing`, never on the `onFrame` driver path (§4 #8/#10); that all-open
  falls through to `doEnterDiscovering` with no spin; and that no `buildBeacon` caller asserts a golden
  byte array, so the encrypted-default flip cannot desync a comparison. It reproduced the fail-before
  itself (neutralising the skip fails exactly the two new tests, the 19 pre-existing cases still pass) and
  re-ran lint (0/0, 64 docs), the generator (INDEX byte-identical), and the full suite (287/287).
