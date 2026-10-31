---
id: '0107'
title: "Full 2.4 GHz channel coverage: hop 1-13 and re-apply the regulatory domain on every WiFi bring-up"
type: architecture
status: accepted
date: 2026-10-31
supersedes: []
superseded_by: []
---

## Context

The shipped autonomous hunt swept only channels **1, 6, 11** (`hunt_loop.cpp`, the default list ADR-0013's
`ChannelHopper` was given). Those are the non-overlapping 2.4 GHz channels most APs cluster on, so 1/6/11
catches the majority of networks for the least airtime. But an AP beacons on *its own* channel, and a
promiscuous sniffer parked on 1/6/11 never hears one on 2-5, 7-10, or 12-13. For an **endless handshake
hunter**, every off-1/6/11 AP is a handshake silently forfeited, and **channels 12-13 are standard under
ETSI (most of Europe)** — in that region the appliance was blind to a large share of the band.

The sibling repo The Adversary hit the regulatory half of this and fixed it the same week: it set the full
1-14 regulatory domain once, but every WiFi mode cycle re-inits the stack and resets the country to the
**1-11 default**, so `esp_wifi_set_channel`/`softAP` on a ch12-13 target failed with `ESP_ERR_INVALID_ARG`,
silently breaking scan, capture and deauth there. Its fix is a `wifi_utils::applyFullChannelRegulatory()`
helper (country JP, `WIFI_COUNTRY_POLICY_MANUAL`, channels 1-14) re-applied after *each* WiFi bring-up.

The Sapper never set a regulatory domain at all, so simply widening the hop list would walk straight into
the same restriction: `esp_wifi_set_channel(12..14)` is rejected under the default 1-11 domain. The one
mercy is that the Sapper already fails **loud** there rather than silently: `Esp32RadioSniffer::setChannel`
returns false and `HuntEngine` skips the target with a logged reason (ADR-0015 decision #5). Loud, but
still no coverage. So widening coverage requires *both* a wider hop list *and* the regulatory domain, re-
applied on every bring-up exactly as the Adversary learned.

## Decision

1. **Sweep the full band 1-13.** The shipped hop list becomes `{1..13}`, named `kShippedHopChannels` in
   `net/channel_hopper.h` so the coverage is greppable and host-pinned against silent reversion. **Ch14 is
   excluded**: it is 802.11b-only and Japan-only and carries no modern APs, so a dwell there is wasted.
   At the ADR-0013 300 ms dwell the discovery sweep period grows from ~0.9 s (3 channels) to ~3.9 s (13),
   acceptable for an autonomous endless hunt; capture is unaffected (the engine tunes to the *target's* own
   channel and holds it for the capture window, not the sweep).

2. **Pin the regulatory domain to 1-14 and re-apply it after every WiFi bring-up.** A device-only helper
   `applyFullChannelRegulatory()` (`net/wifi_regulatory.{h,cpp}`: country JP, `WIFI_COUNTRY_POLICY_MANUAL`,
   `schan=1`, `nchan=14`, carried from the Adversary's `wifi_utils`) is called immediately after each
   `WiFi.mode()` that reaches an arbitrary channel:
   - `Esp32RadioSniffer::begin` — the hot path. It hops 1-13, and the upload supervisor stops then
     re-`begin`s the engine around every STA window (`upload_supervisor.cpp`), so `begin` runs on every
     hunt resume and the re-apply rides it.
   - `connectStation` (`wifi_station.cpp`) — the common STA path for both the boot association and the
     upload/sync window (`Esp32StationControl::bringUpStation` delegates to it), because the configured
     *uplink* may itself be on ch12-13 in Europe, so the STA must be allowed to associate there.
   The re-apply is **mandatory, not a one-time init**: the stack resets the country to the 1-11 default on
   each mode cycle, so applying once is the exact latent bug the Adversary just fixed. The SoftAP portals
   (`CaptivePortal`, `MaintenancePortal`) are **exempt**: they serve on ch1, legal under any domain, so a
   ch12-14 domain buys them nothing.

3. **JP, not US/EU, and the TX consequence.** JP is the only domain whose channel set spans the full 1-14,
   so a MANUAL policy over it never restricts a hop the hunt asks for. The consequence is that the appliance
   **transmits on ch12-14 regardless of its physical location** — the same deliberate trade the Adversary
   documents. This is recorded in the README; lawful use is the operator's responsibility, consistent with
   the shipped-but-disarmed raw-TX posture (§4 #16).

4. **Verification.** The widening is host-pinned: `test/test_channel_hopper` asserts `kShippedHopChannels`
   is exactly 1-13, so a revert to a narrower set fails the native lane. The regulatory helper and its
   bring-up wiring are device-only (`esp_wifi_set_country`), so their correctness is **on-air**: an ETSI-
   region ch12-13 AP discovered and captured, the operator's to run on hardware (review-only + LEDGER, like
   every other device-radio glue row). ADR-0015 decision #5's loud per-channel skip remains the backstop if
   a channel is ever still rejected.

## Consequences

- New device module `net/wifi_regulatory.{h,cpp}` (excluded from the native lane by `build_src_filter`,
  and `#ifndef UNIT_TEST`-guarded like `radio_sniffer_esp32.cpp`). Two bring-up sites gain the re-apply
  (the sniffer `begin`, and `connectStation` for both STA bring-ups).
  `net/channel_hopper.h` gains `kShippedHopChannels`/`kShippedHopChannelCount`; `hunt_loop.cpp` sweeps them
  and its `[HUNT] discovering channels=…` log reads `1-13`.
- New §4 invariant **#44** records the re-apply-on-every-bring-up rule (applying once is forbidden; SoftAP
  exempt), in this ADR's commit (same-commit rule).
- README documents full-band coverage and ch12-14 transmission regardless of location.
- LEDGER carries the on-air proof (an ETSI-region ch12-13 capture needs hardware in that region) and notes
  ch14's deliberate exclusion from the hop list.
- No pure logic changed: `ChannelHopper`, `HuntEngine` and their host tests are untouched (the engine
  already accepts any channel list and copies it); only the shipped list's contents and the device radio
  setup changed. So the native suite is unaffected but for the new pin test.
