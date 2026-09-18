---
id: '0013'
title: "RF channel hopping and beacon-based AP discovery over the fixed-channel seam"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

ADR-0011 built the fixed-channel promiscuous RX seam (`RadioSniffer` + `FrameConsumer`, slice-0012)
and deferred two follow-ons to the ledger: band-sweeping channel hopping "with a dwell-time probe"
and beacon-based AP discovery / target selection. This ADR builds both, on top of that seam, and
records the shape they take so the later HuntEngine (slice-4) and the deauth/station-scanner slices
inherit a settled design rather than re-deciding it.

The 2.4 GHz band the ESP32-S3 radio can reach is 13–14 channels; a promiscuous sniffer parked on one
channel (slice-0012) sees only that channel's traffic. To enumerate the APs in range — the input any
targeted hunt needs — the radio must visit each channel in turn, and something must read the beacons
each visit yields. Three forces shape how:

- **Beacons are periodic, not continuous.** An AP beacons roughly every 102.4 ms (the 100-TU default
  beacon interval). A channel visit shorter than that interval can miss the beacon entirely, so the
  *dwell time* per channel is the design's one measurable parameter — too short and discovery is
  unreliable, too long and a full sweep is slow.
- **The seam forwards raw bytes only (ADR-0009 §4 invariant #8; ADR-0011).** Discovery is beacon
  *interpretation* — reading a BSSID, an SSID, an operating channel out of frame bytes — which is
  exactly the protocol work the pure core owns. It cannot live in the device wrapper without
  re-entangling what ADR-0009 split apart.
- **The RX callback does nothing but forward (§4 invariant #10).** Whatever consumes beacons runs, on
  device, in the same Wi-Fi-driver-task context slice-0012's `HandshakeConsumer` runs in. ADR-0011
  decision #3 already settled that a *bounded, allocation-free* parse in the consumer is within that
  budget (the collector's `ingest` parses there today); discovery must stay inside the same budget.

## Decision

**1. Channel hopping is a pure scheduler over the existing seam; the radio call is the only new seam
method.** A pure `ChannelHopper` (host-testable, no radio) owns the *schedule*: given a channel list,
a dwell time, and a clock, it decides which channel to sit on and when to advance (wrapping at the
end). The *action* — retuning the radio — is one new method on the existing seam,
`RadioSniffer::setChannel(channel)`, implemented on device as a single `esp_wifi_set_channel`. The
sniffer already owns the channel (`begin` takes it), so changing it mid-sniff is the same object's
job, not a new seam. Timing logic stays pure and host-tested; the syscall stays behind the seam. No
separate hop task or timer: the engine (or, until slice-4, the bench probe) drives `tick(now)` from
its existing loop.

**2. AP discovery is a pure `FrameConsumer` — `ApRegistry` — that records the beacons the seam
delivers.** It parses each beacon (reusing the existing `isBeacon`/`frameBssid`/`beaconSsid`, plus a
new `beaconChannel`) and records each distinct BSSID once, with its SSID and operating channel, into
a **bounded** table. It is the sniffer's consumer for a discovery run exactly as `HandshakeConsumer`
was for a capture run (slice-0012). Its `onFrame` does the same bounded, allocation-free parse the
collector already does, so it stays inside §4 invariant #10 (ADR-0011 decision #3): the invariant
forbids *work in the callback function*, not a bounded parse in the pure consumer the callback
forwards to. This keeps all 802.11 interpretation on the green native lane (invariant #8).

**3. An AP's channel comes from the beacon's DS Parameter Set element, not from where we heard it.**
`beaconChannel` reads the DS Parameter Set IE (element id 3), the AP's own statement of its operating
channel. A beacon can bleed onto an adjacent channel during a sweep, so the channel we were *parked
on* is an unreliable source; the beacon's own claim is authoritative. When the IE is absent (rare on
2.4 GHz), the registry falls back to the channel the hopper is currently on — a best-effort
approximation, recorded as such — because a beacon strong enough to decode was heard near that
channel. The fallback is the one cross-task field on the registry (the app task sets the current
channel on each hop; the driver-task `onFrame` reads it), so it is a `std::atomic<uint8_t>` for the
same reason ADR-0011's consumer pointer is atomic.

**4. The registry is bounded and fails loud on overflow — it never silently drops an AP.** The table
is a fixed array (no heap growth in the driver-task context). When it is full and a new BSSID
appears, the registry sets an `overflowed()` flag rather than silently discarding the AP or evicting
an old one; a caller that sees overflow knows its enumeration is incomplete (quality bar §3: no
silent truncation). The bound is a compile-time constant sized for a dense-but-realistic environment.

**5. The dwell time is a configurable parameter with a spec-grounded default; its measured minimum is
lane-3 work.** ADR-0011 named "a dwell-time probe." The measurable question — *what dwell reliably
catches a beacon on every occupied channel* — can only be answered on hardware this design cannot
run, so a standalone probe script would ship un-run with an empty number, which is worse than none.
Instead the dwell is a build-time parameter defaulting to **300 ms** (≈3× the 102.4 ms beacon
interval — enough margin to tolerate one or two missed beacons), and the slice-0014 verify *is* the
measurement: it reports per-channel beacon reception at the configured dwell, so an operator confirms
300 ms suffices or lowers it and re-runs. This folds the probe into the verify's artifact half rather
than a second hardware script (KISS), and the hardware tuning is tracked in the ledger against this
ADR. The default is honestly a spec-derived floor, not yet a hardware measurement.

**6. Discovery enumerates; it does not select or rank.** The registry answers "which APs are in
range." *Choosing* which to hunt and retuning the radio to its channel belongs to the HuntEngine
(slice-4), which also owns the promiscuous-vs-STA radio arbitration (ADR-0009). Ranking APs by signal
strength needs RSSI, which lives in the radio's `rx_ctrl` metadata, not in the frame bytes the seam
forwards — surfacing it would widen `FrameConsumer::onFrame` past "raw frame bytes only" (invariant
#8). Both are deferred to the ledger; this slice keeps the seam pure.

**7. No shipped hopping loop yet (as ADR-0011 decision #5).** There is still no engine to own an
endless sweep or the radio-mode switch an upload needs. The device hop-and-discover path is bench
scaffolding gated behind `SAPPER_TEST_HOOKS` (`rf_discover_probe`), driven by the slice-0014 verify;
it compiles to no-op stubs in every shipped build, so no shipped binary hops or discovers. Sniffing
and hopping are receive-only, so this is not the deauth/upload actuation of §4 invariant #4, but
gating keeps the scaffolding out of every shipped binary.

## Consequences

- `RadioSniffer` gains one pure-virtual method, `setChannel`. Its only implementer is
  `Esp32RadioSniffer`; no test mocks the seam (the pure `ChannelHopper` and `ApRegistry` are tested
  without a radio), so on-air behaviour of `setChannel` is proved by the slice-0014 verify, as
  `begin`/`stop` were by slice-0012's.
- `eapol.h` gains `beaconChannel` alongside `beaconSsid`; both walk the same tagged-parameter list, so
  the walk logic is shared. It is host-tested on the native lane (invariant #8 evidence: the parser
  lives where the tests run, absent from the device wrapper).
- No new `CLAUDE.md` §4 invariant. Hopping and discovery are governed by the invariants ADR-0009 and
  ADR-0011 already set (#8 raw-bytes-only seam, #10 no work in the callback); adding a row for "retune
  only through `setChannel`" would restate #8's boundary discipline without a new obligation, so it is
  left out to keep the table lean.
- The dwell default (300 ms) is a spec-derived floor, not a measurement. The ledger carries the
  hardware-tuning follow-up citing this ADR; when measured, the number amends this ADR (an appended
  `## Amendment`, the one edit an immutable body may take).
- The deauth and station-scanner slices register their own radio callbacks against this same seam;
  this ADR plus ADR-0011 are the contract they follow — pure scheduler, pure consumer, raw-bytes
  forward, no work in the callback — so they do not re-decide it.
