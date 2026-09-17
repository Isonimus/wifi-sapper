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

- [feature] serial control channel for the device-verify harness — read-only ping/state/event-tap unflagged in every build, stimulus/fault injection behind a SAPPER_TEST_HOOKS flag (adapt dupin:ADR-0014; the Sapper has a live deauth/upload path dupin lacks). Underpins every slice's verify script, so land it with or just after slice-1
- [feature] slice-1: HAL skeleton + Board Profile capability struct + LovyanGFX bring-up on Cardputer (native + board build green)
- [feature] slice-2: WiFi STA + NTP time sync + captive AP config portal (WiFi creds + wpa-sec API key → NVS)
- [feature] slice-3: capture engine — port handshake_capture + deauth + station_scanner + pcap writer (headless)
- [feature] slice-4: HuntEngine — extract the endless AutoHunt state machine out of the UI into a headless engine
- [feature] slice-5: tls_upload + wpa-sec upload + persisted retry queue (auto-upload on each new capture)
- [feature] slice-6: wpa-sec cracked-download + hourly scheduler + per-BSSID manifest + new-password alerts
- [feature] slice-7: alert surfaces — web dashboard + LED status codes + optional screen toast + push webhook (ntfy/Discord)
- [feature] slice-8: broaden hardware — CYD / M5Stick / bare-ESP32 board profiles + per-profile memory & palette tuning
- [feature] allowlist-only operating mode for unattended deauth, surfaced in the config portal (atop the ported whitelist)
- [deferred] OTA firmware update for field appliances — scope-heavy, revisit after the capture→sync spine (slices 1–6) is green

## Resolved

Entries move out of "Open" by deletion. A narrative of what merely *happened* belongs in
the git log; what belongs on an open item is evidence it cannot be re-derived later — a
measured result, a reproduction, a count the eventual decision turns on. Keep that with
the item while it is open, and move it into the ADR that closes it. What this file is not
is a record of completed work.
