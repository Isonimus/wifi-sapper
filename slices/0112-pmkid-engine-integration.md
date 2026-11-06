---
id: '0112'
title: "Clientless-PMKID engine integration: unified terminal, the solicit injector, the two arms"
type: slice
status: accepted
date: 2026-11-06
supersedes: []
superseded_by: []
---

## Goal

Make a PMKID M1 a first-class capture on the single capture-to-drain pipeline (ADR-0110
slice 2), building on the pure logic slice-0111 landed. Three moves, one data path:

- **Unify the terminal condition.** `HuntEngine` ends a Capturing window on
  `isWpaSecValid()` OR `isPmkidValid()` and reports the one handshake through the one
  `CaptureReadyObserver`, so a PMKID M1 (solicited, or deauth-forced, or merely overheard)
  is captured, enqueued, and drained exactly like a 4-way (ADR-0110 #1). This is
  **always on**, not gated by an arm: a PMKID M1 is a real, crackable capture however it
  arrived.
- **Carry a PMKID capture through the existing drain to BOTH backends.** The unified
  terminal forces two matching changes with no new seam: `serializeHandshake` must emit a
  beacon+M1 PMKID capture (it is `NotUploadable` today, dropped at the queue — a silent
  lost handshake, §3), and the pwncrack drain must re-derive a `WPA*02` line from the pcap
  when there is no `WPA*01` to send. wpa-sec takes the {beacon+M1} pcap unchanged (ADR-0110
  #7: its server extracts the PMKID), so no new wpa-sec upload path.
- **Add the solicitation parallel injector, and decouple the two arms.** During Capturing,
  an armed engine injects the proven blind pair (open-auth, then an association request
  with a CCMP+PSK RSN IE) on a cadence through the same `RawTransmitter` deauth uses
  (ADR-0110 #2). Arming splits into two independent default-off flags
  (`setDeauthArmed`/`setSolicitArmed`): a non-null transmitter now means only "raw-TX path
  available", and two flags gate what is sent (ADR-0110 #5, the §4 #16 refinement).

Out of scope (slice 3): the `pmkidSolicit` provisioning field, the setup-form checkbox,
the `hunt_loop` solicit-arm wiring, and the on-air armed-solicitation TX + real-PMKID-capture
proof. This slice wires the deauth arm through the new `setDeauthArmed` seam (to keep `main`
coherent the moment the arm semantics change) but leaves the solicit arm reachable only from
the host test until slice 3 provisions it, exactly as slice-0111's pure logic shipped with no
device caller.

## Definition of Done

- **Given** a capture with a beacon and an M1 carrying a PMKID KDE but no M2
- **When** it is offered to the `CaptureQueue`
- **Then** `serializeHandshake` emits the {beacon+M1} pcap and the capture is stored (not
  `NotUploadable`), while a lone M1 or a lone beacon still serializes nothing

- **Given** a stored PMKID capture drained with pwncrack provisioned
- **When** the drain re-derives its hashline from the pcap
- **Then** it builds the `WPA*02` line (no `WPA*01` is derivable) and uploads it to pwncrack,
  and a capture from which neither line is derivable is still counted underivable and does
  not block removal

- **Given** an engine capturing a target whose M1 carries a PMKID KDE (no M2 ever arrives)
- **When** `tick()` sees the PMKID
- **Then** it ends the window, reports the handshake once through the `CaptureReadyObserver`,
  clears that AP's backoff miss count (ADR-0070 #32), and advances to the next target; a
  full 4-way still reports exactly as before

- **Given** a solicit-armed engine in the Capturing phase with a non-null transmitter
- **When** the solicit cadence comes due
- **Then** it transmits an open-auth request, and on a later tick at least the inter-frame
  gap afterwards transmits the association request, repeating on the solicit interval and
  stopping on the first PMKID; a deauth-armed-only engine transmits deauth and no solicit,
  a solicit-armed-only engine transmits solicit and no deauth, and an engine with neither
  arm set (or a null transmitter) transmits nothing

## Design

**Unified terminal (ADR-0110 #1).** The Capturing branch of `tick()` already quiesces
toward `ReportThenAdvance` on `isWpaSecValid()`; it now does so on
`isWpaSecValid() || isPmkidValid()`. Nothing else in the report path changes: the
`ReportThenAdvance` resume already reports `collector_.handshake()` and calls
`registerCaptureOutcome(..., captured=true)`, so a PMKID clears the backoff like any capture
(ADR-0110 #3), and the one-report-per-window structure is the double-save guard by
construction (a window quiesces out of Capturing the instant it reports, so it can never
report twice, whether its terminal was a PMKID M1 or a 4-way). The two predicates are
mutually exclusive in practice (a 4-way M1 carries no PMKID KDE; a solicited PMKID M1 has no
M2), but the report-once structure holds even if both were ever true.

**Carry it through the existing drain (ADR-0110 #7, ADR-0109 #46).** `serializeHandshake`
(net/pcap) today returns false unless `isWpaSecValid()`; it becomes
`isWpaSecValid() || isPmkidValid()`. The record-writing loop is unchanged: it already emits
the beacon then whichever of M1-M4 are present and skips an absent message, so a beacon+M1
capture serializes with no new branch. `parseHandshakePcap` (net/pcap_reader) is already a
generic frame round-trip and reconstructs a beacon+M1 `CapturedHandshake` unchanged — the
slice adds only a round-trip test, no code. The pwncrack drain (`uploadToPwncrack`) tries
`buildHc22000Line` (WPA*01) and, when that yields no line, falls back to
`buildPmkidHc22000Line` (WPA*02) before declaring the capture underivable (ADR-0110 #5e: the
drain tries WPA*01 then WPA*02 by what the capture holds). The wpa-sec leg is untouched: it
uploads the pcap bytes and wpa-sec extracts the PMKID server-side.

**What was considered and reused:** the whole move reuses the existing seams unchanged — the
one `CaptureReadyObserver` -> `CaptureQueue` -> drain -> both backends (ADR-0109 #46), the
pure `isPmkidValid`/`buildPmkidHc22000Line` of slice-0111, and the deauth injector's exact
`RawTransmitter` + tally + cadence shape. No second observer, queue, store, writer, or radio
interface is added — the explicit non-goal of ADR-0110 #1 and ADR-0109 #46.

**The solicit injector.** `maybeTransmitSolicit(now)` sits beside `maybeTransmitDeauth(now)`
in the Capturing tick. The sibling's proven mechanism is auth, a 25 ms inline delay, then
assoc, inside its own task (`adversary:ADR-0138`). Our `tick()` runs on the app loop and must
not block (the HUD, the upload supervisor, discovery timing all share it), so the 25 ms gap
is realised as a **two-phase, non-blocking** cadence instead of a busy-delay: on a due
cadence the injector sends the auth and arms `solicitAssocDueMs_ = now + solicitInterFrameMs`;
on a later tick once that deadline is reached it sends the assoc and arms the next cadence
`solicitDueMs_ = now + solicitIntervalMs`. This is a deliberate deviation from the proven
inline mechanism, justified by the non-blocking-`tick()` constraint; the inter-frame gap
becomes a floor (at least `solicitInterFrameMs`, at most one app-loop tick more), which is
the safe direction (the AP holds the open-auth state well beyond 25 ms). Wrap-safe via the
shared `reached()`, like `deauthDueMs_`. On entering Capturing, `doStartCapturing` seeds
`solicitDueMs_ = nowMs` (fire the first auth on the first Capturing tick, mirroring
`deauthDueMs_`) and clears `solicitAssocPending_`.

The association request needs the target SSID, which the collector does not carry until a
beacon is heard; `doStartCapturing` copies `registry_.at(targetIndex_).ssid` into a
`capturingSsid_` member at retarget (app task, under the §4 #11 settle), so the injector has
it from the first tick. A hidden network (empty SSID) solicits with an empty SSID element
(fail-toward-nothing: the AP will not answer, and a hidden PMKID would have no ESSID for the
`WPA*02` line anyway). Frames are built by the pure `buildAuthRequest`/`buildAssocRequest`
(slice-0111) into a `kSolicitFrameBufLen` stack buffer and handed to `transmitter_->transmit`;
`solicitTxOk_`/`solicitTxFail_` tally the outcome exactly as the deauth counters do, for a
later on-air verify and the host test.

**Decouple the two arms (ADR-0110 #5, the §4 #16 refinement).** `transmitter_ != nullptr`
stops meaning "deauth armed" and means only "raw-TX path available". Two independent flags,
`deauthArmed_` and `solicitArmed_`, both default false, gate what is sent; `maybeTransmitDeauth`
checks `transmitter_ != nullptr && deauthArmed_`, `maybeTransmitSolicit` checks
`transmitter_ != nullptr && solicitArmed_`. Set through `setDeauthArmed`/`setSolicitArmed`,
the same optional-arm shape as `setCrackedLookup`/`setSubmittedLookup`. **What was deliberately
not reused:** the constructor's transmitter-means-armed convention (slice-0030) is dropped,
not extended, because ADR-0110 #5 needs a transmitter present for solicit-only with deauth
off. To keep `main` coherent in the same change, `hunt_loop` now calls
`setDeauthArmed(record.deauthEnabled)` (the transmitter is still injected only when
`deauthEnabled`, until slice 3 adds `|| pmkidSolicit`); without this the shipped loop would
inject a transmitter and never arm it, silently disabling the deauth that §4 #16 ships. The
`pmkidSolicit` provisioning field and `setSolicitArmed` wiring are slice 3, so the solicit
injector is reachable only from the host test here (dormant like slice-0111's pure logic).

The solicit interval (1500 ms) and inter-frame gap (25 ms) are `HuntConfig` fields beside
`deauthIntervalMs`, carried from the sibling's measured as-built values as
architecture-derived defaults; tuning against our 8 s capture window is the existing LEDGER
item. The per-window attempt count is bounded by the capture window itself (no separate cap);
KISS until the field shows it needs one.

## Verification

All four Definition-of-Done scenarios are host-assertable on the native lane (ADR-0004 lane 1);
no `scripts/*-verify.mjs` is owed here (the armed-solicitation on-air TX leg is slice 3's, the
operator's to run).

- `test/test_pcap` (extended): a beacon+M1 PMKID capture serializes a non-empty pcap; a lone
  M1 and a lone beacon still serialize nothing. Covers DoD scenario 1.
- `test/test_pcap_reader` (extended): a serialized PMKID capture round-trips back to a
  `CapturedHandshake` whose `isPmkidValid()` holds and whose `buildPmkidHc22000Line` equals
  the line built from the original handshake (the §4 #46a round-trip, extended to the PMKID
  path). Supports DoD scenario 2.
- `test/test_upload_supervisor` (extended): a stored PMKID capture drains to pwncrack as a
  `WPA*02` line (no `WPA*01` derivable), is counted accepted, and is removed only when both
  backends are terminal; a capture from which neither line derives stays underivable and does
  not block removal. Covers DoD scenario 2.
- `test/test_hunt_engine` (extended): a Capturing window ending on a PMKID M1 reports once and
  advances (and clears backoff); the solicit injector sends auth then assoc a gap later on the
  cadence and stops on a PMKID; the two arms are independent (deauth-only, solicit-only,
  neither, and a null transmitter); the existing deauth tests keep passing with
  `setDeauthArmed(true)`. Covers DoD scenarios 3 and 4.

## As built

Shipped as designed, with one gate the design did not foresee. `serializeHandshake` (net/pcap)
now emits a capture that is `isWpaSecValid() || isPmkidValid()`, and `uploadToPwncrack`
(net/upload_supervisor) falls back to `buildPmkidHc22000Line` when `buildHc22000Line` yields
no line. The unforeseen gate: `parseHandshakePcap` (net/pcap_reader) also rejected a
non-`isWpaSecValid` capture, so the drain's re-parse would have dropped every PMKID before the
WPA*02 fallback ran; it now mirrors the serialize gate (`isWpaSecValid() || isPmkidValid()`),
which the extended `test_pcap_reader` round-trip proves. `HuntEngine` ends a Capturing window
on either terminal and reports through the one observer (the `RecordingObserver` test fake now
accepts a PMKID capture); `maybeTransmitSolicit` sits beside `maybeTransmitDeauth` as the
non-blocking two-phase injector; the two arms decoupled into `setDeauthArmed`/`setSolicitArmed`
(both default off), `deauthArmed_`/`solicitArmed_` gating what a present transmitter sends.
`hunt_loop` now calls `setDeauthArmed(record.deauthEnabled)` so the shipped deauth still arms
after the decoupling (the transmitter is still injected only when `deauthEnabled`). `HuntConfig`
gained `solicitIntervalMs`/`solicitInterFrameMs`; the engine gained `capturingSsid_` (copied at
retarget for the assoc) and `solicitTxOk_`/`solicitTxFail_` tallies.

All four Definition-of-Done scenarios proved on the native lane: `test_pcap` (+2:
PMKID serializes, lone beacon refused), `test_pcap_reader` (+1: PMKID round-trip +
WPA*02-from-pcap equals direct), `test_upload_supervisor` (+1: PMKID drains to pwncrack as
WPA*02), `test_hunt_engine` (+7: PMKID terminal reports once and advances, the two-phase solicit
cadence, stop-on-PMKID, the two arms independent, transmitter-present-but-unarmed is inert, a
refused solicit counts as a fail), plus the five existing deauth tests kept green by arming
deauth explicitly. The solicit arm is reachable only from the host test (no provisioning field
yet); the `pmkidSolicit` provisioning + setup form + `hunt_loop` solicit wiring + the on-air
armed-solicitation TX and real-PMKID-capture proof remain slice 3 (LEDGER). The full native lane
and both tight-budget classic-ESP32 device builds stay green.
</content>
</invoke>
