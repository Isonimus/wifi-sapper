---
id: '0063'
title: "Skip open networks in hunt target selection"
type: architecture
status: accepted
date: 2026-09-22
supersedes: []
superseded_by: []
---

## Context

The endless hunt tunes to every discovered AP in turn, arms a deauth cadence against it, and waits a
full `captureWindowMs` for a WPA/WPA2 4-way handshake. An **open** (unencrypted) network has no 4-way
handshake to capture: the capture window against it always times out empty, having spent airtime and —
when armed — a run of broadcast deauth/disassoc bursts on a target that can never yield a handshake.
The waste is categorical, not marginal: it is one whole capture window (plus deauth TX) per open AP,
every discovery cycle, forever. The Adversary's autohunt already skips these (`WIFI_AUTH_OPEN` in
`selectNextTarget`), so the Sapper hunting them is a regression against its own parent.

The design question is only *how to tell "no capturable handshake" apart from a real WPA network
cheaply, and without ever mis-classifying a WPA network as skippable* — a false skip is a **missed
handshake**, the one failure this appliance exists to avoid, so the criterion must fail toward
capturing.

Three criteria were weighed:

- **Capability Information Privacy bit** (bit 4 of the 2-octet capability field in the fixed beacon
  body). Clear = open; set = *any* encrypted BSS — WEP, WPA1, WPA2, WPA3 alike. One bit at a fixed
  offset, no IE walk. It cannot mis-read a WPA network of any generation as open.
- **RSN IE (0x30) presence.** Would also skip WEP, but **false-skips a pure-WPA1-only AP** (which
  advertises WPA only through the vendor IE, no RSN) — a missed handshake. Correct pure-WPA1 detection
  needs walking *all* 221 vendor IEs (WMM/WPS/WPA can each be a 221), which `findBeaconIe` — returning
  the first match — cannot do, so it would mean a second IE-walk outside the one bounds-checked walk
  the core keeps in one place (`eapol.cpp`), for a case extinct since ~2004.
- **RSN or WPA-vendor IE.** Fully correct for every WPA version including pure WPA1, but carries the
  same new multi-221 walk for near-zero real-world gain.

## Decision

1. **Detect open by the Privacy bit.** A new pure reader `beaconIsOpen(frame, len)` in `eapol` returns
   true only when a readable capability field has the Privacy bit clear; a frame too short to hold the
   field returns false (undecidable → *not* open → still captured). `ApRegistry::onFrame` records the
   result as `DiscoveredAp::isOpen` in the same fully-written entry as bssid/ssid/channel (§4 #10). The
   privacy bit is chosen over RSN parsing precisely because it never false-skips a WPA1/2/3 network.

2. **Skip an open target in selection.** `HuntEngine::doStartCapturing` skips a `DiscoveredAp` whose
   `isOpen` is set — `++targetIndex_; continue;` — exactly like the channel-rejected skip and the gate-B
   cracked skip (ADR-0061). It is therefore never tuned to, never captured, and (never becoming
   `capturingBssid()`) never deauthed. All-open degrades to re-discovery via the existing "no capturable
   target remained" fall-through, never a spin.

3. **Always on, no opt-in.** Unlike the cracked filters (ADR-0059 gate A, ADR-0061 gate B) which hang
   off an optional `CrackedLookup` because they depend on external recovered state, `isOpen` is an
   intrinsic property of the beacon already parsed in the pure core, needs no external state, and is
   read only on the app task in target selection (never in the `onFrame` driver callback), so §4 #8/#10
   hold unchanged. There is no reason an operator would ever want to hunt an open network, so there is
   nothing to gate.

4. **WEP is deliberately not skipped.** Telling WEP from WPA needs the IE parsing decision 1 rejects,
   and its only benefit is the extinct WEP case. A WEP network therefore still attempts capture — a
   bounded wasted window, never a missed WPA handshake. This is the same "fail toward capturing" bias as
   the undecidable-beacon default.

## Consequences

- Standing invariant §4 #29 is added in this commit.
- The `buildBeacon` test builder gains a `bool encrypted = true` parameter and its default flips to
  encrypted (Privacy bit set) — the realistic default for a WPA-capture suite, where the all-zero fixed
  body had made every fixture accidentally "open". No capture-expecting `test_hunt_engine` test needs
  editing (its APs are now encrypted and still targeted); the open case is an explicit opt-in
  (`encrypted=false`) at the skip-open test sites. No production frame layout changes.
- This is the third target-selection filter and the first intrinsic, always-on one: it complements #27
  (upload-side cracked drop) and #28 (targeting-side cracked skip), which are opt-in and external-state
  driven. `DiscoveredAp` gains one in-memory boolean; nothing persisted changes.
- No stale classification: the registry is reset at every re-discovery sweep, so `isOpen` is re-read
  from a fresh beacon each cycle — a network that flips open↔encrypted is re-classified within one
  sweep.
