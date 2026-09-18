---
id: '0011'
title: "The RadioSniffer RX seam: promiscuous 802.11 capture on a fixed channel"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

ADR-0009 fixed the *shape* of the capture path: a pure protocol core fed through HAL seams, the
RF seam forwarding raw bytes only (decision #1, §4 invariant #8). It named the promiscuous RX
seam as "a later RF slice" but did not build it. This ADR builds that seam and records the one
durable, non-obvious rule its implementation must obey — before the deauth and station-scanner
slices, which register their own callbacks against the same radio, are written to the same shape.

The parent Adversary's `HandshakeCapture` is the working reference for *reaching* the frames, and
also the anti-pattern for *structuring* it. Two of its choices are hazards this seam must not
inherit:

- **Logic in the callback.** `esp_wifi_set_promiscuous_rx_cb()` registers a plain C function that
  the Wi-Fi driver invokes **in its own task context**. The reference's callback is a singleton
  method that parses the frame and mutates capture state inline. That context forbids blocking and
  heap allocation, and doing protocol work there both risks the driver and re-creates the
  untestable, hardware-entangled structure ADR-0009 decision #1 rejected. The callback is the one
  place a later operator will be tempted to "just parse it here"; the rule against it has to be
  written at a citable site, not left to memory.
- **A callback with no context argument.** The ESP-IDF RX callback takes no user pointer, so the
  registered frame must reach a chosen consumer through a file-scope pointer. That single static is
  unavoidable and is *not* the singleton ADR-0009 forbade — it holds no protocol state and lives
  only inside the device wrapper — but it must be confined there, behind the pure seam, not spread
  into the core.

The chosen scope for the first RF slice (slice-0012) is a **fixed-channel** promiscuous sniffer:
the honest minimal radio→raw-bytes bridge. Channel hopping and beacon-based AP discovery are real
follow-on work, but folding them in now would re-bundle the four subsystems ADR-0009 deliberately
split apart.

## Decision

**1. The RX seam is `RadioSniffer` + `FrameConsumer`, both pure abstract interfaces.** A
`RadioSniffer` owns promiscuous mode on one channel (`begin(channel, consumer)` / `stop()`) and
hands every received 802.11 frame to a `FrameConsumer::onFrame(frame, len)` as raw bytes. Both
compile on the native lane (ADR-0004 lane 1); the ESP-IDF implementation (`Esp32RadioSniffer`) is
device-only. A host test drives a real `FrameConsumer` through the same `onFrame()` the device
callback calls (§4 invariant #2 — stimulus enters through the seam a real frame uses), so the
seam→core wiring is proved without a radio. The one adapter that bridges the seam to the pure
accumulator is `HandshakeConsumer` (forwards each frame into a `HandshakeCollector`); it carries no
logic beyond the forward.

**2. Coarse frame selection is a radio filter, not code.** The implementation sets
`esp_wifi_set_promiscuous_filter()` to management + data frames, so beacons and EAPOL arrive while
control frames (ACK/RTS/CTS, which never carry either) do not. This is the *hardware* being told
what to deliver — configuration at the seam, not protocol interpretation in the callback — so it
does not violate §4 invariant #8. The callback forwards every frame the radio delivers, dropping
only packet types the IDF marks as non-frame (e.g. `WIFI_PKT_MISC`, whose `sig_len` is not a real
length): a length-validity backstop against an over-read, not protocol filtering.

**3. The RX callback copies and returns — nothing else.** The promiscuous RX callback runs in the
Wi-Fi driver task. It may only copy the raw frame to the registered consumer and return: no heap
allocation, no blocking call, no protocol parsing or capture-state mutation in that context.
Feeding the frame into the pure core is a bounded `memcpy` (the collector copies a matching frame
and returns), which is within that budget; anything heavier belongs on the engine side, after the
frame has left this context. This becomes CLAUDE.md §4 invariant #10.

**4. Fixed channel now; hopping and AP discovery are deferred.** The seam sits on the channel it is
told to watch. Band-sweeping channel hopping (with a measured dwell time) and beacon-based AP
discovery / target selection are follow-on slices, tracked in the ledger, built against this same
seam.

**5. No shipped always-on capture yet.** There is no engine to own an endless capture loop or to
arbitrate the radio between promiscuous sniffing and the STA association an upload needs (ADR-0009
notes those two radio modes are mutually exclusive). Until the HuntEngine (slice-4) owns that
switch, the device sniffer-run path is **bench scaffolding gated behind `SAPPER_TEST_HOOKS`**
(`rf_sniff_probe`), driven by the slice-0012 verify script — never a shipped code path. Sniffing is
receive-only, so it is not the deauth/upload actuation of §4 invariant #4, but gating it keeps the
temporary scaffolding out of every shipped binary.

## Consequences

- CLAUDE.md §4 gains **invariant #10** in the same commit that accepts this ADR: the promiscuous RX
  callback forwards raw frame bytes to the pure core and does nothing else — no allocation,
  blocking, or protocol interpretation in the Wi-Fi-driver callback context. Review-only: the
  linter cannot see "no work in the callback", so `/wrap-up` is its enforcement, and the parser's
  absence from the device wrapper (it lives on the green native lane) is the evidence.
- The single file-scope consumer pointer inside `radio_sniffer_esp32.cpp` is the unavoidable cost
  of the context-free ESP-IDF callback. It is confined to the device wrapper, holds no protocol
  state, and is cleared by `stop()` so no frame is delivered after stop — it is not the singleton
  ADR-0009 rejected.
- The lane-3 verify for this slice proves that **real frames reach the core** (a known AP's beacon,
  captured on the fixed channel), not that a full 4-way handshake becomes a wpa-sec-valid pcap on
  air. The latter needs an emission/storage path this slice deliberately does not build; it is
  deferred to the slice that owns that path (slice-5 upload) and named there, against this core.
- Deauth and the station scanner will register their own radio callbacks. This ADR is the contract
  they follow — pure seam, raw-bytes forward, no work in the callback — so they do not each
  re-decide it.
