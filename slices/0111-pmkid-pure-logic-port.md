---
id: '0111'
title: "Port the clientless-PMKID pure logic: solicitation builders, PMKID extractor, WPA*02 line"
type: slice
status: accepted
date: 2026-11-06
supersedes: []
superseded_by: []
---

## Goal

Land the pure, host-tested half of clientless PMKID (ADR-0110 slice 1), with **no device
consumer**, so the engine integration (slice 2) and the provisioning arm + wiring + on-air
proof (slice 3) build on proven pieces, exactly as ADR-0108 landed the `WPA*01` line ahead of
its pwncrack consumers. Four pure deliverables:

- `net/pmkid` : `extractPmkidFromEapol(eapol, len, out[16])`, the PMKID KDE reader (tag 0xDD,
  OUI 00-0F-AC, type 0x04, Key Data at offset 95 past the key descriptor), carried from
  `adversary:src/utils/pmkid_utils.h` whose offsets were proven on-device 2026-11-04.
- `net/pmkid_solicit` : `buildAuthRequest` + `buildAssocRequest` + `defaultSolicitStaMac`, the
  blind-pair frame builders carried from `adversary:src/utils/pmkid_solicit.h`, byte-exact to
  the frames that drew M1 on the real chip on 2026-11-04.
- `net/hc22000` : `buildPmkidHc22000Line`, the `WPA*02*PMKID*AP*STA*ESSID***` hashline beside
  the existing `WPA*01` line. This folds the **pure half** of the deferred `WPA*02` hc22000
  LEDGER item.
- `net/handshake_collector` : `CapturedHandshake::isPmkidValid()`, the second terminal
  condition (beacon + an M1 carrying a PMKID KDE) beside `isWpaSecValid()` (ADR-0110 #1).

Nothing in this slice transmits, provisions, or wires into the hunt loop: the frame builders
have no caller, `isPmkidValid()` is read by no engine yet, and the `WPA*02` line is fed to no
drain. Those are slices 2 and 3.

## Definition of Done

- **Given** an EAPOL M1 whose Key Data carries a PMKID KDE
- **When** `extractPmkidFromEapol` reads it
- **Then** it returns true and the 16-byte PMKID, while an M1 with no KDE, empty Key Data, a
  truncated frame, an over-long Key-Data-Length, or a null pointer returns false and reads
  nothing out of bounds

- **Given** a target BSSID, a spoofed STA MAC, and an SSID
- **When** `buildAuthRequest` and `buildAssocRequest` build the blind pair
- **Then** the bytes equal the proven-on-device 2026-11-04 layout exactly (frame control,
  addresses, the CCMP/CCMP/PSK RSN IE), an SSID over 32 octets clamps to 32 with a matching
  element-length byte, and a null or too-small buffer returns 0 rather than a short frame

- **Given** a capture holding an M1 that carries a PMKID KDE and a non-empty ESSID
- **When** `buildPmkidHc22000Line` runs
- **Then** it writes `WPA*02*<pmkid>*<ap>*<sta-from-M1-Addr1>*<essid>***` and returns true,
  while a missing M1, an M1 with no PMKID KDE, an empty ESSID, or a too-small buffer returns
  false and leaves no partial line

- **Given** a capture with a beacon and an M1 carrying a PMKID KDE but no M2
- **When** `isPmkidValid()` is queried
- **Then** it returns true, while a capture with a beacon and an M1 that carries no PMKID KDE
  returns false, and a full 4-way capture still reports `isWpaSecValid()` unchanged

## Design

The two pure headers already exist and are host-tested in the sibling; the work is porting
them onto our namespace and build, not inventing logic. **What was considered and reused:** the
sibling's `pmkid_utils.h` and `pmkid_solicit.h` are lifted near-verbatim into `sapper`, keeping
the measured byte layout and offsets (they are the proven artifact: a reordered field or a
shifted offset silently stops soliciting or extracting on hardware, where it is expensive to
notice), renamed only to our `k`-prefixed constant convention and split into `.h`/`.cpp` like
`net/deauth` rather than kept header-only. The extractor takes the EAPOL header pointer
`locateEapol` (`net/eapol`) already returns, so `isPmkidValid()` and `buildPmkidHc22000Line`
both compose it with the existing frame readers at build time: no new interpretation in the
collector or the driver callback (ADR-0009 #3, §4 #8/#10 hold, like ADR-0108's `WPA*01` path).

The `WPA*02` line reuses the field order and lowercase-hex of `handshake_save.h`'s proven PMKID
writer (`adversary`), which cracks: `WPA*02*PMKID*MAC_AP*MAC_STA*ESSID***`, the three trailing
fields (ANONCE, EAPOL, messagepair) empty because a PMKID line has no 4-way. The STA is read
from **M1's Addr1** (frame offset 4: M1 is AP->client, so Addr1 is the addressed station), which
is our spoofed solicit MAC in the solicited path and a real client in the deauth-forced path,
single-sourced from the frame with no per-path branch (ADR-0110 #6), mirroring how the `WPA*01`
line single-sources its STA from M2's Addr2.

**What was deliberately not reused:** the sibling's `handshake_save.h` save pipeline and its
`HandshakeCapture` solicitation state machine, both SD/device-coupled; our save path is the
seam-decoupled `CaptureReadyObserver` -> queue -> drain, so the state machine and the arm are
slice 2/3 on our seams, not a lift. The bounded hex/string appenders in `buildPmkidHc22000Line`
are the **second** use of the pattern in `buildHc22000Line`; left duplicated at two per the rule
of three (a third hashline type extracts a shared file-local helper, LEDGER), and the proven,
on-air `buildHc22000Line` is left byte-for-byte untouched.

**Deferred to slice 2 (noted, not decided here):** whether a PMKID-only capture feeds wpa-sec
as a {beacon + M1} pcap (ADR-0110 #7's default, since our collector keeps both frames whole) or
is pwncrack-only (the sibling's choice, which never builds a PMKID pcap). This is a drain-wiring
question with no pure-logic surface, settled when slice 2 touches the pcap/upload path and
amended into ADR-0110 only if on air shows wpa-sec rejects a PMKID-from-pcap.

No magic offsets escape a named constant; the measured EAPOL offsets carry the same provenance
comment as `hc22000.cpp`.

## Verification

All four deliverables are pure and fully host-assertable, so no `scripts/*-verify.mjs` is owed
(the active-TX on-air leg is slice 3's, and is the operator's to run). Unit tests on the native
lane (ADR-0004 lane 1):

- `test/test_pmkid_utils` (new): `extractPmkidFromEapol` against a hardcoded wire layout
  (offsets written independently of the header so a wrong offset fails here) : extracts a PMKID,
  skips a preceding non-PMKID element, and the fail-closed paths (no KDE, empty Key Data,
  truncated frame, over-long Key-Data-Length, null arguments). Covers DoD scenario 1.
- `test/test_pmkid_solicit` (new): byte-exact `buildAuthRequest`/`buildAssocRequest` against the
  2026-11-04 vectors, the 32-octet SSID clamp, the too-small-buffer and null-pointer zero
  returns, and the locally-administered default STA MAC. Covers DoD scenario 2.
- `test/test_hc22000` (extended): the exact `WPA*02` line against a known PMKID M1, the STA
  taken from M1's Addr1, and fail-closed on a no-KDE M1, an empty ESSID, and an undersized
  buffer (no partial line). Covers DoD scenario 3.
- `test/test_handshake_collector` (extended): `isPmkidValid()` true on beacon + a PMKID M1,
  false on beacon + a non-PMKID M1 and on an M1 without a beacon, and `isWpaSecValid()`
  unchanged for a 4-way. Covers DoD scenario 4.

A shared `buildPmkidM1(bssid, client, pmkid)` is added to `test/support/frame_builders.h` (its
second/third consumer, its documented purpose) so the PMKID M1 fixture is single-sourced across
the two extended suites.

## As built

Shipped exactly as designed. Added `src/net/pmkid.{h,cpp}` (`extractPmkidFromEapol`, `kPmkidLen`),
`src/net/pmkid_solicit.{h,cpp}` (`buildAuthRequest`, `buildAssocRequest`, `defaultSolicitStaMac`,
`kSolicitFrameBufLen`/`kSolicitAuthFrameLen`/`kSolicitSsidMaxLen`), `buildPmkidHc22000Line` in
`net/hc22000` (`WPA*02*PMKID*AP*STA*ESSID***`, STA from M1 Addr1, appenders duplicated at two),
and `CapturedHandshake::isPmkidValid()`. Wired `net/pmkid.cpp` and `net/pmkid_solicit.cpp` into
the `[env:native]` `build_src_filter`. Added `buildPmkidM1` to the shared frame-builder support
header. All four DoD scenarios proved by the two new and two extended native suites; the full
native lane and both tight-budget classic-ESP32 device builds stay green (the pure additions
link into every board, no consumer yet). No frame is transmitted and no provisioning field is
added in this slice. The on-air solicitation proof and the per-backend drain wiring remain
slices 2-3 (LEDGER).
</content>
