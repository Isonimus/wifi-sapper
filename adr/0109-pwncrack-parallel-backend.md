---
id: '0109'
title: "pwncrack.org as a parallel second cracking backend (sibling seams over one queue)"
type: architecture
status: accepted
date: 2026-11-02
supersedes: []
superseded_by: []
---

## Context

The decided direction (LEDGER) is a **parallel second cracking backend**: every capture uploads to
*both* wpa-sec and **pwncrack.org**, and cracked results sync from both, for the best crack odds across
different engines and wordlists. ADR-0108 shipped the first half, the pure on-device WPA\*01 `.hc22000`
synthesis pwncrack consumes (wpa-sec takes a raw `.pcap` and builds the hashline server-side; pwncrack
takes the hashline). The pwncrack API was re-verified firsthand on 2026-11-02 (upload
`POST /upload_handshake`, multipart `key` + `handshake` file, **JSON** response; results
`GET /download_potfile_script?key=`, colon-potfile with BSSID index 1, ESSID 3, password 4; HTTPS/443
works via Cloudflare with a Google Trust Services cert already in our pinned bundle). This ADR fixes the
remaining spine, the uploader/classifier/cracked-sync/provisioning seams, before any of it is built.

The existing spine is hard-bound to wpa-sec's contract, and three facts in the corpus shape how a second
backend attaches:

- **The retry queue persists serialized *pcap bytes*, not the whole frames.** `CaptureQueue::offer()`
  serializes the live `CapturedHandshake` to a pcap at capture time and persists only the pcap
  (`CaptureStore`, LittleFS-backed, survives reboots). At drain time, possibly after a reboot, the
  supervisor has pcap bytes in hand, never the `CapturedHandshake.msg[0..3]` frames `buildHc22000Line`
  (ADR-0108) consumes. So the pwncrack hashline cannot simply be built "from the capture" at drain; it
  has to come from the one artifact that *is* persisted, the pcap, which does contain the Beacon + M1 + M2
  as whole 802.11 frames (pcap.h), everything the WPA\*01 line needs.
- **The `Uploader` seam is pcap-shaped, and the cracked/submitted gates are single-backend.** `Uploader`
  takes `(pcap, len, key)` and returns `UploadResult`; the `UploadSupervisor` removes a queued capture on
  one terminal success; the `SubmittedManifest` (gate C, ADR-0065) and the `CrackedManifest` (gates A/B,
  ADR-0059/0061) each track one backend. A second backend with a *different* payload (hashline, not pcap),
  a *different* upload response (JSON, not hcx text), and *independent* per-BSSID progress does not fit the
  single-backend assumptions those pieces were built under.
- **The sibling The Adversary's proven shape is parallel service singletons, not a polymorphic backend
  interface** (`adversary:src/modules/network/pwncrack_service.*` sits beside the wpa-sec service as a
  second concrete service, reusing a shared result enum, no abstract backend type). That is the
  duplicate-don't-abstract answer the quality bar's rule of three prescribes at the **second** instance.

pwncrack is also **optional**: the appliance's primary backend is wpa-sec (the `.pcap` path, no on-device
hashline needed), so a device provisioned with only the wpa-sec key must keep behaving exactly as it ships
today. pwncrack attaches only when its own key is provisioned, like the optional push webhook (ADR-0023).

## Decision

1. **Parallel sibling seams, not a polymorphic backend interface.** Add an `Hc22000Uploader` seam beside
   the pcap `Uploader` (`upload(const char* hc22000Line, const char* key) -> UploadResult`) and a
   `PwncrackResultsFetcher` beside the wpa-sec `CrackedResultsFetcher`, both reusing the existing
   `UploadResult` / `FetchResult` enums (the one shared vocabulary the Adversary also reuses). No abstract
   "CrackingBackend" type: it would have to unify a pcap payload with a hashline payload and a text
   classifier with a JSON classifier across exactly **two** concrete backends, which is the speculative
   generality the rule of three forbids at n=2. A third backend is the trigger to extract an interface,
   not the second (LEDGER already frames onlinehashcrack as a separate, non-this-item candidate).

2. **The pwncrack hashline is re-derived at drain time from the persisted pcap**, by a new pure,
   host-tested module `net/pcap_reader` (`parseHandshakePcap(bytes, len) -> CapturedHandshake`), whose
   result is fed to ADR-0108 `buildHc22000Line` **unchanged** (DRY: one hashline builder, one hashline
   test). The reader composes the existing pure readers (`isBeacon`, `frameBssid`, `beaconSsid`,
   `locateEapol`, `identifyMessage`) to classify each pcap record back into a `CapturedHandshake`, so it
   adds **no new 802.11/EAPOL interpretation outside the pure core** (§4 #8 holds) and the collector still
   parses nothing on-device (ADR-0009 #3 holds: this is build-time derivation at upload, exactly like
   `buildHc22000Line`, not capture-time parsing). This was chosen over building and persisting the hashline
   at capture time (operator decision, 2026-11-02): re-deriving touches **none** of the proven
   `CaptureStore`/`CaptureQueue` format or the wpa-sec path (zero migration, zero regression surface), and
   spends the derivation only at drain and only when pwncrack is provisioned. Its one cost, a parser that
   must mirror the pcap *writer* or silently drift, is bought off by a **serialize-then-parse round-trip
   test**: a `CapturedHandshake` serialized by `serializeHandshake` and parsed back must reproduce the same
   frames/BSSID/SSID, and a truncated or magic-wrong buffer must fail loud (no partial handshake).

3. **One capture queue, one drain; both backends uploaded in the same STA window; a capture is removed
   only when every *provisioned* backend has terminally taken it.** The drain uploads each pending capture
   to wpa-sec (pcap) and, when pwncrack is provisioned, to pwncrack (hashline from decision #2) inside the
   one STA window the supervisor already opens. A queue entry is deleted only once **both** provisioned
   backends are terminal for it (Accepted or Duplicate) or its BSSID is cracked; removing it on *either*
   backend alone would discard the capture for the backend that has not taken it yet (a lost handshake,
   quality bar §3). With only wpa-sec provisioned, "every provisioned backend" is one, so the removal rule
   is byte-for-byte today's behaviour.

4. **Per-backend submitted-state.** Gate C (ADR-0065) becomes per-backend via a **second
   `SubmittedManifest` instance** for pwncrack (not a backend tag threaded through the existing manifest):
   a second instance keeps the shipped single-backend `SubmittedManifest` code and its tests byte-for-byte
   and matches decision #1's duplicate-don't-abstract shape. The drain consults and records each backend's
   own submitted-set, so a backend that already holds a capture is not re-uploaded while the other retries,
   and the both-terminal removal of decision #3 reads the two sets. The pwncrack submitted-set gets its own
   `SubmittedStore` namespace; the reverification cadence (ADR-0072) applies to it unchanged.

5. **One shared `CrackedManifest`.** A recovered password is backend-agnostic, so both fetchers feed the
   **same** manifest and gates A/B (ADR-0059/0061) are unchanged: a crack found by pwncrack suppresses
   re-capture and re-upload to *both* backends, and vice versa. `CrackedSync` gains the pwncrack fetcher as
   a second source run in the same sync window; the pwncrack colon-potfile (BSSID index 1, ESSID 3,
   password 4) parses through a sibling of the pure `cracked_result_parser` if its field order differs from
   wpa-sec's (decided in the sync slice, by the two layouts). The atomic fetch-then-apply and the
   first-sync-silent-seed (ADR-0019) stay **per-manifest**, not per-source: only the very first sync of a
   fresh manifest seeds silently, and a `New`/`Changed` entry from either source fires the one `NewPassword`
   alert. No alert or manifest field becomes backend-aware.

6. **A pure JSON upload-response classifier (`PwncrackResponseClassifier`), the ADR-0076 analogue.** It is
   fail-closed exactly as the wpa-sec classifier: Accepted / Duplicate only on a recognised positive JSON
   shape, everything else (non-2xx, unparseable body, unrecognised shape, transport/TLS failure) Rejected
   so the capture is kept and retried, never assumed uploaded. It parses **JSON**, not substrings, into a
   **bounded fixed stack buffer** (no heap allocation proportional to the reply: the no-PSRAM ESP32-S3 TLS
   memory constraint that drove ADR-0076 applies identically; a body exceeding the bound is Rejected, not
   heap-grown). Streaming is *not* required here (the pwncrack upload reply is a small JSON object, not
   hcx's multi-KB verbose summary), so the classifier parses a complete bounded body rather than feeding
   chunks. **The exact success/duplicate JSON schema is pinned by an operator-run live probe** with a real
   key (a secret), like the wpa-sec response probe (`artifacts/wpasec-response-probe.txt`): until that
   lands, the classifier's field mapping is provisional and its test fixtures are frozen only after the
   probe, so no fixture encodes a guessed schema (quality bar §3, "measure, don't assume").

7. **A second, optional provisioning key.** `ProvisioningRecord` gains `pwncrackKey`, NVS-backed through
   the single `loadProvisioning()`/`persistProvisioning()` seam (ADR-0006 #6), accepted-but-never-echoed in
   the setup form like the wpa-sec key (§4 #20 by construction: `SetupFormModel` gains a
   `hasStoredPwncrackKey` boolean, never the string). `validateCredentials` stays gated on the **wpa-sec**
   key only (the primary backend); an empty `pwncrackKey` simply **disables** the pwncrack backend (its
   uploader and fetcher are not wired), so a wpa-sec-only device is unchanged. pwncrack is additive and
   fail-safe: a missing or rejected pwncrack key never blocks a wpa-sec upload or a sync.

## Consequences

- A new §4 standing invariant (#46) records the parallel-backend boundary so future work does not collapse
  the sibling seams into a premature interface, "optimise" the drain to remove a capture before both
  backends have it, persist the hashline or move its derivation into the collector, make the shared cracked
  manifest backend-aware, or let a missing pwncrack key degrade the wpa-sec path. Its enforcement is
  `pending (LEDGER)` until the implementing slices land and can cite real tests; the row names the tests
  each slice will add.
- The work splits into slices, each pure-and-host-tested where it can be (ADR-0004 lane 1): (1) `pcap_reader`
  + the round-trip/hc22000-from-pcap test; (2) `Hc22000Uploader` + `PwncrackResponseClassifier` (the
  classifier's fixtures gated on the live JSON probe); (3) the dual-backend drain + the second
  `SubmittedManifest`; (4) `PwncrackResultsFetcher` + the potfile parse into the shared manifest; (5) the
  `pwncrackKey` provisioning field + setup-form wiring. Each lands its §4 enforcement and flips the pending
  row's relevant clause.
- Two gates stay open in the LEDGER before the uploader/classifier can be *finished* (not before the design
  or the earlier slices): the operator-run pwncrack upload-response JSON probe (decision #6), and an on-air
  run behind Cloudflare to confirm no first-contact bot-challenge/rate-limit on the real upload/fetch.
- The wpa-sec path, the pcap format, the capture queue/store, and the existing cracked/submitted manifests
  are untouched by this ADR: every change is additive behind the optional pwncrack key, so a wpa-sec-only
  device needs no on-air re-verification of existing behaviour.

## Amendment - 2026-11-05: pwncrack upload-response schema pinned by probe

Decision #6 deferred the exact success/duplicate JSON schema to an operator-run live probe. The probe
(`scripts/pwncrack-response-probe.mjs`, four legs) ran on 2026-11-05; the redacted schema is in
`artifacts/pwncrack-response-probe.txt`. Measured results:

- **Accept and re-upload both return HTTP 200 `{"success": true}`** (16-byte JSON). So pwncrack has **no
  duplicate signal**: a fresh submit and its immediate re-submit are indistinguishable. This is harmless -
  `UploadResult::Accepted` and `Duplicate` are already terminal-equivalent (capture off our hands), so
  pwncrack simply never yields `Duplicate` and the enum is unchanged. Decision #6's "Accepted / Duplicate"
  wording was a reasonable guess carried from wpa-sec; the measured reality is **Accepted-only**.
- **A bad key returns HTTP 400 `{"error": "Invalid session key."}`** (positive token absent). Auth is
  validated **synchronously**, so a misprovisioned pwncrack key fails loud: Rejected, kept-and-retried, and
  surfaced as repeated pwncrack rejects, never a silent submitted-mark. This was the decisive fail-open
  worry behind running the negative legs, and it is resolved.
- **A malformed hashline also returns HTTP 200 `{"success": true}`.** Content is validated
  **asynchronously** server-side, so the upload reply cannot distinguish a crackable line from junk. The
  probe's blunt mechanical verdict ("the positive token leaked into a negative leg, design must change")
  was examined and found **not** to require a change here, because: (a) the firmware only ever uploads a
  well-formed `WPA*01` line from `buildHc22000Line` (ADR-0108) over a wpa-sec-valid capture, re-derived via
  the round-trip-tested `pcap_reader` (decision #2) - it never produces the leg-3 junk; and (b) pwncrack is
  the **secondary parallel** backend under both-terminal removal (§4 #46 b), so wpa-sec classifies
  independently and still holds the capture. A residual accept-mismatch therefore costs **no handshake**,
  unlike wpa-sec where a false-accept dropped the only copy (the ADR-0076 hazard does not transfer to a
  secondary backend). The leg-4 bad-key case - the one that *would* be dangerous - correctly returns a
  non-positive reject, so the trust boundary that matters holds.

**Finalized classifier contract** (`PwncrackResponseClassifier`, slice 2): **Accepted** on HTTP 200 with a
JSON body whose `success === true`; **Rejected** on everything else - any non-200 (the 400 bad-key case), a
non-JSON body (a Cloudflare HTML challenge), a JSON body without `success:true` (e.g. `{"error": ...}`), a
body over the bounded stack buffer, or a transport/TLS failure - kept and retried, never assumed uploaded.
There is **no Duplicate branch**. The bounded-buffer / no-heap discipline of decision #6 stands (the body
is tiny, 16-32 bytes, but the buffer is fixed and an over-long body is Rejected, not heap-grown). The
`test/test_pwncrack_response` fixtures are frozen on these four measured bodies.

**Cloudflare first-contact (LEDGER gate), workstation leg:** all four legs returned the pwncrack app's JSON
through `server: cloudflare` with `cloudflare-challenge: false` and no interstitial, so the workstation leg
is clean. The device on-air leg (a different TLS client) still confirms on the slice-2/4 run.

## Amendment - 2026-11-06: pwncrack device on-air leg verified

The slice-5 build was flashed to an M5StickC Plus2 (classic ESP32) and provisioned through the captive
portal with a real pwncrack key. The device-leg unknowns the written `uploader_pwncrack.cpp` /
`cracked_fetcher_pwncrack.cpp` were deferred against (the LEDGER gate) all resolved in the operator's
favour: the pinned GTS Root R4 chain validated the live Cloudflare-fronted endpoint (no device/region edge
cert mismatch), the response bodies parsed with no `Transfer-Encoding: chunked` framing to de-chunk, and no
bot-challenge interstitial was served to the device TLS client. Both legs ran green on air: the pwncrack
`upload_handshake` POST was accepted and the `download_potfile_script` sync pulled the potfile into the
shared manifest. So the fail-closed handling (Transport/Rejected + `[ERROR]` on a chain mismatch or a
chunked body) stays as a safety net but was not exercised; the LEDGER device-leg item is closed. This does
not change any decision above; it records that the deferred firsthand device verification passed.

## Amendment — 2026-11-07: hc22000 type tag corrected (the 4-way line is WPA*02, not WPA*01)

This ADR describes the 4-way EAPOL `.hc22000` line from `buildHc22000Line` (ADR-0108) as `WPA*01`. That
tag is wrong: the hashcat 22000 spec numbers the type field 01=PMKID, 02=EAPOL, so the 4-way line is
`WPA*02` and `WPA*01` is the PMKID line. The shipped builders had the two swapped (uncrackable pwncrack
path) and were corrected; see ADR-0108's amendment for the detail. No decision in this ADR depends on the
tag number (the parallel-backend structure, the drain, the shared cracked manifest are all unchanged); this
note only corrects the incidental tag label so a reader here is not misled.
