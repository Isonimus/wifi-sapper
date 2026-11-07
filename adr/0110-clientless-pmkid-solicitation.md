---
id: '0110'
title: "Clientless PMKID solicitation as a parallel injector in the endless hunt"
type: architecture
status: accepted
date: 2026-11-06
supersedes: []
superseded_by: []
---

## Context

The endless hunt backs off a never-captured AP after `kBackoffMissThreshold` empty capture
windows (§4 #32, ADR-0070). A large class of those APs are PMK-caching WPA2-PSK APs with no
associated client: the `field-2026-10-01` run measured 343 of 1330 windows (26%) spent on
APs that never yield a 4-way, because deauth can only force a handshake when a client is
present to reassociate. Such an AP yields nothing to a deauth hunt but will answer an
injected association with an EAPOL M1 carrying a PMKID KDE, which is a crackable `WPA*02`
artifact on its own: the PMKID is `HMAC-SHA1(PMK, "PMK Name" | AP_MAC | STA_MAC)`, so if we
are the station the AP addresses M1 to, the PMKID is keyed on our own chosen MAC and the
hashcat `WPA*02` STA is ours, not a real client's.

The sibling Adversary shipped this on our exact ESP32-S3 + precompiled-IDF (Bruce) stack:

- `adversary:ADR-0137` chose **injected association over a managed STA connect**: the
  precompiled Bruce `wpa_supplicant` runs the 4-way internally and exposes no raw-M1 / PMKID
  hook, and promiscuous mode does not run concurrently with a managed connection on this
  driver, so a managed connect can never surface the PMKID to us. Injected association stays
  in promiscuous mode throughout and reuses the softAP TX netif already raised for deauth.
- `adversary:ADR-0138` is the as-built after the on-device spike **PASSED 2026-11-04**:
  against a known PMKID-emitting AP, a **blind pair** (inject open-auth, wait ~25 ms, inject
  an association request carrying an RSN IE advertising CCMP + AKM PSK, from a spoofed
  locally-administered STA MAC, repeated on an interval) drew an M1 with a PMKID KDE that
  `recordPmkid` stored keyed on the spoofed STA, and the saved `WPA*02` line cracked. All
  three legs of the acceptance condition held. The blind pair, not an await-response
  handshake, is the measured-correct mechanism.
- `adversary:ADR-0139` folded it into **Auto Hunt** (their multi-target capture-anything
  mode, the direct analog of our endless hunt) as a **parallel frame injector** inside each
  per-target window under a Deauth / PMKID / Both strategy, saving + counting + advancing on
  a PMKID, with a per-target guard so the PMKID writer and the 4-way writer never both fire.

Two pieces of pure, host-tested logic are portable: the frame builders
(`adversary:src/utils/pmkid_solicit.h`: `buildAuthRequest`, `buildAssocRequest`) transmitted
through our `RawTransmitter` (§4 #15), and the extractor
(`adversary:src/utils/pmkid_utils.h`: `extractPmkidFromEapol`, KDE tag 0xDD / OUI 00-0F-AC /
type 0x04, Key Data at offset 95 past the key descriptor) run at build time off the whole M1.

Our architecture differs from the sibling's in the way that shapes this decision. The
Adversary has UI-driven top-level handshake modes (`Clientless`, `Auto Hunt`, `Manual`) and a
screen-subscriber save path that was hard-coupled to the 4-way (`saveHandshake` bails at
`isMinimumValid`, which needs M2), which is precisely why `adversary:ADR-0138` had to add a
**separate** PMKID save writer driven off a `PMKID_CAPTURED` event. We have none of that: one
headless `HuntEngine` (= their Auto Hunt), one `CaptureReadyObserver` seam (ADR-0001), one
`CaptureQueue`, and one drain STA window feeding both backends (ADR-0109 #46). Our
`HandshakeCollector` already keeps M1 whole (`msg[0]`) and `net/hc22000` already synthesises
the `WPA*01` 4-way line; the `WPA*02` PMKID line was deferred (ADR-0108, LEDGER). So we port
the pure logic and the Auto-Hunt integration (`adversary:ADR-0139`), not the focused
single-target Clientless mode nor its separate save writer, which the headless Sapper has no
use for.

## Decision

Add clientless PMKID solicitation to the endless hunt as a **parallel frame injector**,
armed by a new default-off provisioned toggle, feeding a solicited PMKID M1 through the one
existing capture-to-drain pipeline as one more valid capture. Seven load-bearing decisions.

### 1. A solicited PMKID is one more valid capture, not a second path

`CapturedHandshake` gains `isPmkidValid()` (has beacon + M1 present + `extractPmkidFromEapol`
on that M1 succeeds), and the engine declares a capture window a win on `isWpaSecValid()`
**or** `isPmkidValid()`. The existing `CaptureReadyObserver` -> `CaptureQueue` -> drain ->
both backends then carries a PMKID capture unchanged.

Rationale: our save path is already seam-decoupled, so the second-writer split
`adversary:ADR-0138` needed (because its save path was 4-way-coupled) buys us nothing and
would reintroduce exactly the second-path complexity ADR-0109 #46 fought to keep off the
backends (one queue, one drain). KISS. The extraction runs at build time in the pure,
host-tested core off the whole M1 the collector already keeps, adding no interpretation to
the driver callback, so ADR-0009 #3 and §4 #8 hold exactly as they do for ADR-0108's
`WPA*01` extraction.

### 2. Solicitation is a parallel frame injector in the per-target Capturing window

Ported from `adversary:ADR-0139`, not a separate mode. In `Capturing`, when the solicit arm
is set, the engine injects the proven **blind pair** on a cadence (open-auth, a short inline
`solicitInterFrameMs` ~25 ms wait, then the association request carrying the RSN IE), bounded
to a per-window attempt count derived from the solicit interval and the capture window,
stopping on the first PMKID. It reuses the same `RawTransmitter` deauth uses (the softAP TX
netif / `esp_wifi_80211_tx`): no new radio interface, the TX mirror of how
`maybeTransmitDeauth` already works.

No RX-side auth/association-response gating is added (`adversary:ADR-0138` decision 1): the
blind pair is the mechanism that drew M1 on the real chip on 2026-11-04, response-parsing in
the promiscuous path is unproven-necessary code, and adding it now is speculative generality
against a path never seen to fail. The ~25 ms inter-frame wait stays inline (once per solicit
interval, shorter than the loop cadence) and runs on the app task in `tick()`, never in the
driver callback, so §4 #10 holds.

### 3. Stop on the first PMKID, then advance; the single-report discipline is the double-save guard

A solicited PMKID is a crackable win for that AP, so the engine reports it and advances to the
next target rather than burning the rest of the window (`adversary:ADR-0139`). Our engine
already reports each captured window through the `CaptureReadyObserver` exactly once, so that
one-report-per-window discipline *is* the sibling's `handshakeSaved_` double-save guard: a
window reports once whether its terminal was a PMKID M1 or a 4-way, and the PMKID and 4-way
never both enqueue for one target. A PMKID capture clears the never-captured backoff miss
count (§4 #32) like any capture: full rehabilitation.

### 4. Arming is two independent default-off provisioned arms, not a per-hunt selector

A headless appliance has no per-hunt UI, so `adversary:ADR-0139`'s Strategy row becomes
provisioning: the existing `deauthEnabled` plus a new default-off `pmkidSolicit`, both
NVS-backed through the `loadProvisioning()`/`persistProvisioning()` seam (§4 #6) and rendered
as a no-echo stored-flag checkbox in the setup form (§4 #20, like the deauth/notify toggles).
Their four combinations give the sibling's strategy: neither = passive, deauth-only = Deauth,
solicit-only = PMKID, both = Both (solicit and deauth concurrently in the window, which
behaves as PMKID-first with deauth as the fallback that keeps working for the rest of the
window if no AP answers solicitation). Each arm is the operator's responsibility like §4 #16:
solicitation injects auth/association frames impersonating a station, a new active-TX
behaviour beyond deauth, so it ships disarmed and transmits nothing until explicitly armed.

### 5. The RawTransmitter injection is decoupled from the deauth arm (a §4 #16 refinement)

Today a non-null `transmitter_` *means* "deauth armed" (§4 #16). This ADR refines that: the
injected transmitter means "the raw-TX path is available," and two independent arm flags
(deauth, solicit) gate what is actually transmitted, so a PMKID-only or Both device transmits
solicitation with deauth off. A wpa-sec-only / deauth-only / passive device stays
byte-for-byte unchanged: with `pmkidSolicit` off nothing solicits, and with the transmitter
null nothing transmits at all. This **refines, does not supersede, §4 #16**: the default-off,
operator-armed, shipped-in-every-binary posture is unchanged, and a deauth frame still
transmits only when deauth is armed; only the one conflated flag splits into two so a second
active behaviour can be armed independently.

### 6. The pwncrack WPA*02 line is synthesised in the pure net/hc22000 module

Beside the `WPA*01` line and host-tested like it (closing the `WPA*02` LEDGER item): a
`WPA*02*<pmkid:32>*<ap:12>*<sta:12>*<essid>` line from a PMKID-bearing M1, with the PMKID from
the ported `extractPmkidFromEapol`, the AP from the record BSSID, the **STA from M1's Addr1**
(the station the AP addressed M1 to, which is our spoofed solicit MAC in the solicited path
and a real client in the deauth-forced passive path, single-sourced from the frame either
way, as the `WPA*01` line single-sources its STA from M2's Addr2), and the ESSID from the
record. A hidden-SSID capture (empty ESSID) yields no `WPA*02` line (uncrackable, like
`WPA*01`; the NUL-SSID LEDGER limitation carries over). The drain's pwncrack feed tries the
`WPA*01` build first (a 4-way capture) and falls back to the `WPA*02` build (a PMKID-only
capture): **the capture determines which, not a mode flag**. No PMKID is ever persisted (the
collector keeps none); it is re-derived from the saved pcap at drain time exactly like the
`WPA*01` re-derivation (ADR-0109 #46a, via `net/pcap_reader`).

### 7. The wpa-sec feed is the pcap, unchanged

A PMKID capture's pcap is {beacon + M1}; wpa-sec's server `hcxpcapngtool` extracts the
`WPA*02` from it, so there is no new wpa-sec upload path (the device-side hashline build is
pwncrack's alone, as for `WPA*01`). The count-aware response classifier
(`net/wpasec_response`) already reads the "written to 22000 hash file" count on every line, so
a "0 PMKID ... written" reply classifies Rejected (host-tested, `test_wpasec_response`); the
real PMKID success wording is confirmed on air (LEDGER).

## Consequences

- A new §4 standing invariant (#47) is added in this ADR's commit for the arming boundary and
  the unified capture model: the `pmkidSolicit` arm, the parallel-injector integration, the
  stop-on-PMKID-then-advance with the single-report double-save guard, the
  `WPA*02`-re-derived-from-pcap chain, and the §4 #16 two-flag refinement. Its clauses start
  `pending (LEDGER)` and flip to `verified_by` as the slices land, like ADR-0109 #46.
- The pure logic lands first with no consumer (slice 1), mirroring how ADR-0108 landed the
  `WPA*01` line: the frame builders, the extractor, and the `WPA*02` hc22000 line, all
  host-tested, so the engine integration (slice 2) and the provisioning arm + wiring + on-air
  proof (slice 3) build on proven pieces. The active-TX on-air leg (does a real PMKID-capable
  AP answer the blind pair on our build) is the operator's to run, like every other
  device-radio proof; the sibling's 2026-11-04 spike is strong prior evidence but not a proof
  on *this* firmware (LEDGER).
- The STA-MAC-is-ours semantics must not be lost, but needs no per-path branch: because the
  `WPA*02` STA is sourced from M1's Addr1, it is correct in both the solicited (our MAC) and
  the passive deauth-forced (real client) paths. A future reader must not hardcode a
  "STA = client" assumption into the `WPA*02` line.
- A cheaper passive precursor falls out for free: `isPmkidValid()` fires on any M1 carrying a
  PMKID KDE, whether we solicited it or a deauthed client produced it, so even a Deauth-only
  hunt banks opportunistic PMKIDs. Solicitation only adds *reaching* the client-less APs that
  deauth cannot.
- The solicitation constants (interval, inter-frame gap, per-window attempt bound, the spoofed
  STA MAC) start as the sibling's measured as-built values; tuning them against our hunt's
  window cadence is deferred (LEDGER), like dwell / deauth-cadence / backoff.
</content>
</invoke>

## Amendment — 2026-11-07: PMKID hashcat type tag corrected (the PMKID line is WPA*01, not WPA*02)

Clause 6 and this ADR's body tagged the PMKID line **`WPA*02`** and the 4-way line `WPA*01`. Both are
backwards: the hashcat 22000 spec numbers the type field **01 = PMKID, 02 = EAPOL** (confirmed against the
[hashcat wiki](https://hashcat.net/wiki/cracking_wpawpa2) and an hcxtools sample PMKID line
`WPA*01*<pmkid>*<ap>*<sta>*<essid>***`, 2026-11-07). So `buildPmkidHc22000Line` must emit **`WPA*01`** and
the 4-way `buildHc22000Line` **`WPA*02`** (the latter corrected on ADR-0108). The shipped builders had them
swapped, which made every pwncrack hashline mislabelled and so uncrackable (wpa-sec is unaffected, it takes
the pcap). Corrected in both builders, with the tests asserting the spec tags (fail-before/pass-after) and
§4 #45/#47 updated. The drain still tries the 4-way line first then the PMKID line by what the capture
holds (now `WPA*02` then `WPA*01`); every other decision here, the solicitation injector, the two arms, the
re-derive-from-pcap chain, the unified single drain, is unchanged.
