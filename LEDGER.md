# Ledger

The single mutable file in this repo (stele:ADR-0001). Everything else is
immutable or generated. Open work, deferrals, and known defects all live here — there is
no second tracking file, because two files require manual sync and manual sync does not
happen.

**Closing an item means deleting its line here.** Do not annotate the source ADR; the
ADR's claim ("at the time of this decision we deferred X") stays true forever and needs
no update. Single writer, one direction.

Format: `- [type] description (ADR-NNNN)` — type is `bug` | `feature` | `deferred` |
`audit`. Cite the source ADR where one exists; rule 8 checks that the citation resolves.
A decision that lives in **another** repo is cited `<repo>:ADR-NNNN` — the linter cannot
open that corpus, so it skips qualified references (stele:ADR-0009).

## Open

<!-- One line per open item. Delete the line to close it; the git log is the done-record.
     Slices below are the initial roadmap; each earns its ADR(s) as it is picked up, and the
     ADR citation is added to its line then (rule 8 rejects a citation that does not resolve). -->

- [feature] serial control channel — STIMULUS half only: fault/stimulus injection behind SAPPER_TEST_HOOKS (synthetic handshake via the capture seam, stubbed wpa-sec responses, forced heap/time faults, hourly-sync clock advance), compiled out of release (ADR-0003). The observation half shipped in slice-0005; land the stimulus half with the engine it drives (slice-3/4)
- [deferred] flash-encryption / secure-boot for secrets at rest — plaintext NVS is the slice-2 baseline; the key is readable from a flash dump until this lands (ADR-0006)
- [feature] optional SD/LittleFS seed-NVS-on-first-boot for boards with storage — a power-user provisioning path alongside the portal (ADR-0006)
- [feature] physical re-provision trigger (button/keyboard-hold at boot) — needs an input HAL the repo does not have yet; slice-0007 shipped only the automatic STA-fail fallback into the portal, so this is the manual override (ADR-0006)
- [feature] capture: promiscuous-vs-STA radio arbitration — slice-0016's HuntEngine selects a target (round-robin) and retunes to it, but does not switch the radio to station mode; the arbitration to pause the hunt, bring the STA up to upload, and resume belongs to slice-5, where the uploader that forces a second radio mode exists to test it against (moved from slice-4, ADR-0015 decision #7)
- [feature] capture: hop dwell hardware tuning — slice-0014's per-channel dwell defaults to a spec-grounded 300ms (≈3× the 102.4ms beacon interval), not yet a measurement; run verify:rf-discover on hardware to confirm/lower the reliable minimum and amend ADR-0013 with the measured number (ADR-0013)
- [feature] capture: RSSI-based AP/target ranking — needs widening the FrameConsumer seam to carry rx_ctrl metadata (RSSI), deliberately out of scope in slice-0014 to keep the seam to raw frame bytes (§4 invariant #8); add when target selection needs signal strength (ADR-0013)
- [deferred] on-air handshake→pcap proof — slice-0012's lane-3 verify proves real frames reach the core (beacon-reception); the full "real 4-way handshake → wpa-sec-valid pcap on air" proof belongs to the slice that owns the emit/storage path (slice-5 upload), where a byte sink exists to write it (ADR-0011)
- [feature] capture: deauth — pure deauth/disassoc frame building + raw-TX seam to force handshake renegotiation, gated by the SAPPER_TEST_HOOKS actuation rule (ADR-0009, ADR-0003)
- [feature] capture: station scanner — passive client discovery for targeted deauth of connected clients (ADR-0009)
- [deferred] PMKID (clientless) capture — a second capture mode alongside the 4-way handshake, deferred to keep the capture core to one path (ADR-0009)
- [feature] capture: hunt quiesce settle hardware tuning — slice-0016's phase-transition settle (before a sink reset/re-target) defaults to an architecture-derived ceiling over a microsecond memcpy callback, not yet a measurement; run verify:hunt on hardware to confirm/lower it from the observed phase timing and amend ADR-0015 (ADR-0015)
- [feature] slice-5: tls_upload + wpa-sec upload + persisted retry queue (auto-upload on each new capture); also builds the promiscuous-vs-STA radio arbitration the HuntEngine hands off to (ADR-0015)
- [feature] slice-6: wpa-sec cracked-download + hourly scheduler + per-BSSID manifest + new-password alerts
- [feature] slice-7: alert surfaces — web dashboard + LED status codes + optional screen toast + push webhook (ntfy/Discord)
- [feature] slice-8: broaden hardware — CYD / M5Stick / bare-ESP32 board profiles + per-profile memory & palette tuning (for the M5StickC Plus2, reuse the on-device-tested board definition from the dragon-ball-radar repo rather than deriving one fresh)
- [feature] allowlist-only operating mode for unattended deauth, surfaced in the config portal (atop the ported whitelist)
- [feature] wire cloud CI to run ADR-0004 lanes 1–2 on push (npm run test:native + build:boards) — backs §4 invariant 5 that no CI lane depends on attached hardware; device verify (lane 3) stays a hardware-runner/manual step (ADR-0004)
- [feature] expand README.md — it now carries the build/verify commands and the first-boot provisioning guide (slice-0007); still to add are the serial observation protocol reference and per-board bring-up/flashing steps (stele:ADR-0010)
- [deferred] OTA firmware update for field appliances — scope-heavy, revisit after the capture→sync spine (slices 1–6) is green

## Resolved

Entries move out of "Open" by deletion. A narrative of what merely *happened* belongs in
the git log; what belongs on an open item is evidence it cannot be re-derived later — a
measured result, a reproduction, a count the eventual decision turns on. Keep that with
the item while it is open, and move it into the ADR that closes it. What this file is not
is a record of completed work.
