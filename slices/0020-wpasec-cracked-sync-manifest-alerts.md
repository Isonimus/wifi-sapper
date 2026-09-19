---
id: '0020'
title: "wpa-sec cracked-results sync — full-account download, per-BSSID manifest, and delta alerts"
type: slice
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Goal

Close the appliance's core loop by bringing cracked results *back*: once an hour, download the account's
entire cracked set from wpa-sec, mirror it into a durable per-BSSID manifest, and announce every
genuinely-new recovered password as an event (ADR-0019). A pure line parser turns the `?api&dl=1` body
into records, a bounded `CrackedManifest` behind a `CrackedStore` seam holds the full account mirror and
reports on each applied result whether it was new, changed, or unchanged (that per-result outcome *is*
the delta — there is no separate detector to drift from it), and a clock-driven `SyncScheduler` decides
*when* — piggybacking the STA window the slice-0018 drain already brings up (ADR-0017 decision #6),
threaded in through a thin `SyncSession` seam so the supervisor shares its one associate. This completes the
capture→upload→sync→alert spine (slices 1–6): after it, the appliance hunts, uploads, and reports cracks
unattended.

It mirrors the **whole account**, not just this device's captures (the deliberate divergence from the
Adversary reference, ADR-0019 decision #2), and fixes that reference's colon-in-password truncation
(decision #3). The first sync of a fresh manifest seeds silently and emits one summary rather than a
burst of backlog alerts (decision #5).

It does **not** drive any UI (headless — ADR-0001; the new-password/summary/outcome events are what
slice-7's surfaces subscribe to), transmit deauth (still deferred behind the §4 invariant #4 actuation
gate), or open its own schedule of associates (it shares the drain's STA session, ADR-0017 decision #6).
Encryption-at-rest for the manifest's plaintext PSKs stays under the existing deferred flash-encryption
ledger item (ADR-0006).

## Definition of Done

**Scenario A — a password containing colons survives the parse verbatim (host).**
- **Given** the pure `CrackedResultParser` and a body line
  `aabbccddeeff:1122334455ee:CoffeeShop:p@ss:w0rd:!`
- **When** the line is parsed
- **Then** it yields one `CrackedResult` with BSSID `aa:bb:cc:dd:ee:ff`, essid `CoffeeShop`, and password
  `p@ss:w0rd:!` **intact** — the parser splits on the first three colons only and keeps the remainder
  verbatim, fixing the reference's `strtok_r` truncation (ADR-0019 decision #3).
- **Proof:** `npm run test:native` → `test/test_cracked_parser`.

**Scenario B — a malformed line is skipped and counted, never partially stored (host).**
- **Given** a body mixing valid lines with a line of fewer than three colons, a line whose AP BSSID is
  unparseable, and a line with an empty essid or password
- **When** the body is parsed
- **Then** every valid line yields a record, each malformed line yields **no** record and increments the
  reported malformed count, and no record is ever emitted from a partially-parsed line — a wpa-sec format
  change surfaces as a non-zero malformed count, not a silent parse-to-nothing (§3; ADR-0019 decision #3).
- **Proof:** `npm run test:native` → `test/test_cracked_parser`.

**Scenario C — the manifest is bounded and per-BSSID; a known result is a no-op, overflow drops the oldest crack (host).**
- **Given** a `CrackedManifest` over a fake `CrackedStore`, holding `kMaxCrackedEntries` entries with
  distinct `firstSeenCrackedMs`
- **When** the same BSSID+password already held is applied again, then a brand-new BSSID is applied
- **Then** the re-applied known result changes nothing (count unchanged, no eviction), and the new BSSID
  evicts the entry with the **oldest** `firstSeenCrackedMs` (count stays at the bound) — wpa-sec still
  holds the record and a re-download re-seeds it (ADR-0019 decision #4).
- **Proof:** `npm run test:native` → `test/test_cracked_manifest`.

**Scenario D — a BSSID re-cracking to a new password updates the entry and is announced (host).**
- **Given** a non-fresh manifest already holding a BSSID with password `old`
- **When** a sync applies the same BSSID with password `new`
- **Then** the stored entry's password becomes `new` (one entry, not two), and the manifest's apply
  outcome for that result is `Changed` (not `New` or `Unchanged`) — a rotated PSK is an update plus an
  alert, not a duplicate (ADR-0019 decisions #4, #5).
- **Proof:** `npm run test:native` → `test/test_cracked_manifest`.

**Scenario E — the first sync of a fresh manifest seeds silently and emits one summary (host).**
- **Given** a manifest whose persisted `freshManifest` flag is set (never yet synced) and a parsed set of
  N results
- **When** the sync applies the set
- **Then** all N are stored, **zero** new-password events are raised, exactly **one** summary event
  (`imported N previously-cracked results`) is emitted, and the `freshManifest` flag is cleared and
  persisted — the backlog does not become N alerts (ADR-0019 decision #5, force 2).
- **Proof:** `npm run test:native` → `test/test_cracked_sync`.

**Scenario F — after the first sync, only genuinely new or changed results are announced (host).**
- **Given** a non-fresh manifest seeded with several results
- **When** a later sync brings a body whose lines are: some already-held unchanged, one brand-new BSSID,
  one held-BSSID with a changed password
- **Then** exactly **two** new-password events are raised (the new BSSID and the changed one), the
  unchanged results raise nothing, and **no** summary event is emitted — steady-state alerting is
  edge-triggered on the manifest apply outcome (ADR-0019 decision #5).
- **Proof:** `npm run test:native` → `test/test_cracked_sync`.

**Scenario G — a genuine first crack into an empty non-fresh manifest is announced, not misfiled as backlog (host).**
- **Given** a manifest whose `freshManifest` flag was already cleared by a prior sync that found an empty
  account, so the manifest is empty **and** non-fresh
- **When** a sync brings exactly one result
- **Then** it raises **one** new-password event (not a summary) — "fresh" is the persisted flag, not
  "manifest empty", so the first real crack is not mistaken for backlog (ADR-0019 decision #5).
- **Proof:** `npm run test:native` → `test/test_cracked_sync`.

**Scenario H — the scheduler marks sync due hourly, wrap-safe, and shares the drain's STA window (host).**
- **Given** a pure `SyncScheduler` on a fake clock, last successful sync recorded
- **When** the clock advances past `kSyncIntervalMs`, including across a `millis()` rollover
- **Then** the scheduler reports the sync **not** due before the interval and **due** after it (the
  wrap-safe deadline holds across rollover); when a drain cycle associates while a sync is due, the sync
  runs inside that same STA window before teardown; and when the upload queue stays empty long enough that
  no drain associates but the hour comes due, the supervisor opens **one** STA window for the sync alone
  — never a second concurrent STA path, never once per capture (ADR-0019 decision #6, ADR-0017 decision #6).
- **Proof:** `npm run test:native` → `test/test_sync_scheduler` (the pure hourly/wrap-safe timing),
  `test/test_upload_supervisor` (piggyback a due sync on a drain's window; force one window for a due sync
  with an empty queue, rate-limited; run a sync only inside a live window), and `test/test_sync_session`
  (a successful sync advances the cadence, a failed one leaves it due for the next window).

**Scenario I — a fetch/parse failure records no successful sync and leaves the manifest untouched (host).**
- **Given** a fake `CrackedResultsFetcher` returning a transport failure, then (on a later cycle) a
  truncated/garbage body
- **When** each cycle runs
- **Then** no cycle updates `lastSuccessfulSyncMs` (so the scheduler retries on the next window rather
  than waiting a full hour), the manifest is left exactly as it was, a sync-outcome event carries the
  failure, and no new-password or summary event is raised — a failed sync fails loud and changes nothing
  (§3; ADR-0019 decision #7).
- **Proof:** `npm run test:native` → `test/test_cracked_sync`.

**Scenario J — on-air: a real `?api&dl=1` fetch over the pinned chain de-chunks, parses, and seeds the manifest (lane 3).**
- **Given** a Cardputer flashed with the `cardputer_testhooks` build, provisioned with real WiFi
  credentials and a real wpa-sec key, with an account holding at least one cracked result
- **When** `npm run verify:sync` drives the device (the clock-advance stimulus forces a due sync without
  waiting a real hour)
- **Then** the device associates, syncs NTP, opens a TLS connection to `wpa-sec.stanev.org` that validates
  against the **pinned GTS Root R4** (a cert-validation failure is a `[FATAL]`), issues
  `GET /?api&dl=1` with the key cookie, reads the body through `HTTPClient` so any
  `Transfer-Encoding: chunked` framing is de-framed (no split line), parses it into records with the
  reported malformed count, seeds the LittleFS manifest, emits the first-sync summary, and returns to
  hunting — proving the download→de-chunk→parse→persist path end to end on real hardware against the real
  service. The run fails on any `[FATAL]`/`[ERROR]`; the artifact records the `[SYNC]` lines (downloaded,
  new, malformed counts) and the observed TLS/associate/fetch timing.
- **Proof:** `scripts/0020-sync-verify.mjs`, wired as `npm run verify:sync`.

## Design

**The pure line parser.** `src/net/cracked_result_parser.{h,cpp}` — turns one body line into a
`CrackedResult { uint8_t bssid[6]; char essid[kEssidCap]; char password[kPasswordCap]; }`
(`src/net/cracked_result.h`). Splits on the **first three** colons only, keeps the remainder as the
password verbatim (ADR-0019 decision #3), and classifies a line as `Ok`, `Empty` (blank/whitespace,
ignored), or `Malformed` (fewer than three colons, unparseable AP BSSID, empty essid/password — skipped
and counted). Owns no buffers beyond the record it fills; host-tested line-by-line and over a captured
body. Line-oriented by design so the device backing can stream lines off the HTTPClient reader without
buffering the whole account on the heap.

**The `CrackedStore` seam.** `src/net/cracked_store.h` — `load(entries&)` / `save(entries)` plus the
persisted `freshManifest` flag and `lastSuccessfulSyncMs`, each reporting failure (§3). Device backing
`src/net/cracked_store_littlefs.{h,cpp}` (device-only, `#ifndef UNIT_TEST`) serializes the manifest to one
LittleFS file (the portable floor, present on every target, surviving reset — ADR-0019 decision #4); the
host lane uses `test/support/fake_cracked_store.h` (in-RAM, with an injectable failure for Scenario I).
This is the seam the new §4 invariant names: cracked passwords are read and written only through it.

**The pure manifest policy.** `src/net/cracked_manifest.{h,cpp}` — the bounded, per-BSSID map
(AP BSSID → `{ essid, password, firstSeenCrackedMs }`) over the `CrackedStore`. Decides *what* is held,
which known result is a no-op, and which **oldest** entry to evict on overflow (`kMaxCrackedEntries`,
by `firstSeenCrackedMs` — the same recency bound and rationale as the capture queue). Owns no filesystem;
host-tested through the fake store. `apply(result, nowMs)` returns an `ApplyOutcome` of `New`, `Changed`,
or `Unchanged`: **that per-result outcome is the delta.** ADR-0019 originally described a separate
delta-detector unit; building one would mean a second lookup-and-compare that must stay in lockstep with
the manifest's own — the exact duplication the quality bar's DRY rule forbids. The detector is therefore
folded into `apply`: the sync announces on `New`/`Changed` and stays silent on `Unchanged`, so there is
one source of truth for "is this new" and nothing to drift.

**The `CrackedResultsFetcher` seam.** `src/net/cracked_fetcher.h` — `fetch(const char* key, LineSink&)`
returning a `FetchResult` (`Ok` / `Transport` / `Auth` / `Empty`). Device backing
`src/net/cracked_fetcher_wpasec.{h,cpp}` (device-only) opens a `WiFiClientSecure` with the pinned root CA
(`src/net/wpasec_root_ca.h` — the same measured GTS Root R4 PEM as the upload, ADR-0017 decision #1),
NTP-gated exactly as the upload is, and reads `GET /?api&dl=1` with the key cookie **through
`HTTPClient`** so the stack de-chunks the body (ADR-0019 decision #1 — a split line here corrupts data,
not just costs a retry, so this is not a raw-TLS read); it hands each line to the `LineSink`. The host
lane uses a fake replaying a captured body's lines, or a scripted failure.

**The sync orchestration (pure policy + injected seams).** `src/net/cracked_sync.{h,cpp}` — holds a
`CrackedResultsFetcher&`, a `CrackedManifest&`, the wpa-sec key, and a `SyncEventObserver&`. One
`runSync()`: fetch → parse each streamed line into a fixed result buffer (counting malformed/overflow) →
**only on an Ok fetch with a usable body**, apply each result to the manifest, recording which came back
`New`/`Changed` → flush the manifest once → on the **first** sync of a fresh manifest, seed silently and
emit one summary, else emit one new-password event per recorded `New`/`Changed` → always emit a
sync-outcome event (downloaded, new, malformed, overflow, network result, storeError). Buffering the
*parsed* results (not the raw body) and applying only after a confirmed-Ok fetch is what makes a transport
failure or an all-malformed body leave the manifest exactly as it was (Scenario I) while still streaming
off the HTTPClient reader; the flush precedes any announcement so no crack is announced that was not
durably recorded (§3). Fully host-tested through the fake fetcher, fake store, and a recording observer.

**The pure scheduler.** `src/net/sync_scheduler.h` (header-only — pure and tiny) — a clock-driven,
wrap-safe (`static_cast<int32_t>(nowMs - deadlineMs) >= 0`) unit marking a sync **due** once
`kSyncIntervalMs` has elapsed since the last successful sync; `noteSynced(nowMs)` on success. Fake-clock
host-tested like every other window in the engine.

**The `SyncSession` seam and its binding.** `src/net/sync_session.h` — an abstract `SyncSession`
(`begin`/`due`/`runInWindow`) that the supervisor drives, and the header-only `ScheduledSyncSession` that
binds the pure `SyncScheduler` to the pure `CrackedSync`. The seam exists so the supervisor's
window-sharing logic is host-tested against a trivial fake (`test/support/fake_sync_session.h`) instead of
the whole sync stack, exactly as `StationControl`/`Uploader` are. Its one piece of logic beyond delegation
is the decision-#7 cadence rule — `noteSynced()` only on a successful `runSync()` — kept here so neither
the supervisor nor the scheduler needs to know the other.

**Wiring into the drain's STA window.** The `upload_supervisor` (slice-0018) already stops the hunt,
brings the STA up, and tears it down per drain cycle. slice-6 gives it an optional `SyncSession*`: when a
window opens for any reason and a sync is due, `runInWindow()` runs before teardown (piggyback); when the
queue is empty but the hour comes due, the supervisor opens one window for the sync alone, **rate-limited
by the existing drain time ceiling** so a persistently-failing sync retries on a cadence, not every tick
(ADR-0019 decision #6). The sync's success/failure is deliberately **not** folded into the upload backoff
— a wpa-sec download outage must not drag out upload cadence; a failed sync simply stays due (decision #7)
and offline is already handled by the shared `bringUpStation()` backoff. No second STA path; the
promiscuous↔STA transition stays the supervisor's alone. Under `SAPPER_TEST_HOOKS` a clock-advance
stimulus forces a due sync for the verify (the STIMULUS half the ledger already tracks); that vocabulary
stays out of any shipped binary (§4 invariant #4).

**Events.** Three additions to the existing event bus: a new-password event (BSSID, essid, password), a
first-sync summary event (imported count), and a sync-outcome event (downloaded/new/malformed/network
result). slice-6 only *emits*; slice-7 subscribes (ADR-0019 decision #7, ADR-0001).

**The download probe.** `scripts/wpasec-download-probe.mjs` captures one live `?api&dl=1` response (the
operator holds the only key that can) and reports its structural stats — line count, the colon-count
distribution, and how many lines the parser would reject — so the contract is pinned by measured numbers,
not prose (ADR-0019 decision #3). The raw body carries the account's real ESSIDs and recovered PSKs, so it
is written to the gitignored `src/gen/` (the same place the upload verify's real pcap lands) and **never
committed**; the parser's host tests use synthetic-but-format-accurate lines, and the measured numbers land
in the record. Probes are exempt from the R11 wiring rule.

## Verification

- **Native Unity tests** (Scenarios A–I) — `test/test_cracked_parser` (colon-safe split, malformed
  skip-and-count), `test/test_cracked_manifest` (bound, per-BSSID no-op, oldest-eviction, password update,
  and the `New`/`Changed`/`Unchanged` apply outcome that *is* the delta), `test/test_cracked_sync`
  (fresh-seed-and-summarise, steady-state deltas, empty-non-fresh first crack, fetch/parse failure leaves
  state untouched, flush-failure announces nothing), `test/test_sync_scheduler` (hourly due, wrap-safe),
  `test/test_sync_session` (cadence advances only on success), and the window-sharing cases in
  `test/test_upload_supervisor` (piggyback, forced rate-limited sync-only window, live-window-only, no
  faked activity). All drive the pure units through the fake fetcher, fake `CrackedStore`, a fake
  `SyncSession`, a recording observer, and a fake clock. Wired as `npm run test:native`; runs in cloud CI
  (ADR-0004 lane 1). The pinned-CA validation, the real `?api&dl=1` contract, and HTTPClient de-chunking
  are not host-assertable and are proved by Scenario J.
- **On-air verify** (Scenario J) — `scripts/0020-sync-verify.mjs`, wired as `npm run verify:sync` (R11).
  Runs on a workstation with a provisioned board attached, never in cloud CI (§4 invariant #5); its
  error-check half (fail on any `[FATAL]`/`[ERROR]`, require a TLS handshake against the pinned root, a
  `200` de-chunked body, a non-negative parse with its malformed count, a seeded manifest, and a return to
  hunting) is machine-checkable, and it writes the observed `[SYNC]` counts and TLS/associate/fetch timing
  as the review artifact.
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) links the shipped
  `.elf` with the fetcher, manifest store, scheduler, and sync orchestration present; the device-only
  seams compile under the ESP toolchain and the pure policy shares the native lane. The report records the
  flash delta (the HTTPClient path over the already-linked TLS stack, plus the manifest store).

## As built

Shipped as designed, host-proven on the native lane (45 tests across the slice), board-compiled on the
shipped and test-hooks builds (RAM 35.0%, Flash 37.5% on `cardputer`), and proven on air against live
wpa-sec. Deviations from the plan above, recorded per the freeze step:

- **The delta detector is folded into the manifest, not a separate unit.** `CrackedManifest::apply()`
  returns `New` / `Changed` / `Unchanged`, and that per-result outcome *is* the delta the sync announces
  on. A standalone `cracked_delta` unit would have re-derived the same BSSID lookup-and-compare the
  manifest already does — two code paths that must agree, the exact duplication the quality bar's DRY
  rule forbids — so it was not built. Coverage lives in `test/test_cracked_manifest` (the outcome
  classification) and `test/test_cracked_sync` (that it drives the alerts).
- **A `SyncSession` seam was added beyond the planned file list.** `src/net/sync_session.h` — an abstract
  `SyncSession` (`begin`/`due`/`runInWindow`) the supervisor drives, plus the header-only
  `ScheduledSyncSession` binding the scheduler to `CrackedSync`. It exists so the supervisor's
  window-sharing logic is host-tested against a trivial fake (`test/support/fake_sync_session.h`) rather
  than the whole fetcher+manifest+store stack, exactly as `StationControl`/`Uploader` are. Its only logic
  beyond delegation is the decision-#7 cadence rule (`noteSynced()` only on success), proven in
  `test/test_sync_session`.
- **The scheduler and the session are header-only** (`sync_scheduler.h`, `sync_session.h`): pure, tiny,
  and all-inline, so no `.cpp` and no native `build_src_filter` entry, like other small pure units.
- **The forced sync-only window is rate-limited by its own `syncRetryIntervalMs`, decoupled from both the
  upload backoff and `maxDrainIntervalMs`** (an adversarial-review hardening). A due sync opens its own
  STA window when the upload queue is empty, but its success/failure is deliberately kept out of the
  upload backoff — a wpa-sec download outage must not drag out upload cadence — and its retry cadence is
  a dedicated knob, not the upload trickle ceiling, so raising that ceiling for a low-activity deployment
  cannot starve the hourly sync. A failed sync stays due (decision #7) and retries on the next window;
  offline is handled by the shared `bringUpStation()` backoff. Proven in `test/test_upload_supervisor`
  (piggyback, forced rate-limited window, independence from a long upload ceiling, live-window-only, no
  faked activity).
- **The device fetcher reads the body through `HTTPClient::writeToStream()`, not `getStreamPtr()` (a
  hardware finding).** ADR-0019 decision #1 requires de-chunking; the reference's `getStreamPtr()`
  approach returns the *raw* client, which does **not** de-chunk — on a `Transfer-Encoding: chunked` body
  it would surface chunk-size headers as lines and split a result across a chunk boundary, the exact
  corruption decision #1 exists to prevent. `writeToStream()` de-chunks (identity **and** chunked) and,
  critically, returns a negative error on any incomplete transfer, which is also what stops a truncated
  account from ever being reported `Ok` (Scenario I). It feeds a small line-splitting `Stream` that emits
  whole lines to the sink, buffering one line at a time, not the whole account.
- **The LittleFS store replaces the manifest by `rename()` alone (an adversarial finding).** An earlier
  `remove()`-then-`rename()` opened a window where a rename failure or a power loss — the routine event
  this store survives — left `/cracked.bin` gone and `/cracked.tmp` not yet in place, which the next boot
  would read as a fresh empty manifest and silently lose the history. `LittleFS.rename()` overwrites the
  destination atomically, so renaming straight over the target keeps the prior manifest intact on any
  failure.
- **Measured download format (probe, ADR-0019 decision #3).** `scripts/wpasec-download-probe.mjs` against
  a live account: 27 lines, 26 cracked results, **every** content line exactly three colons, **zero**
  rejected by the parser's rules — confirming the `ap_bssid:client_bssid:essid:password` contract and the
  split-on-first-three-colons decision by measured bytes, not prose. The raw body (real ESSIDs/PSKs) is
  written to gitignored `src/gen/` and never committed; the parser's host tests use synthetic
  format-accurate lines.
- **Scenarios A–I** (host): met exactly by `test/test_cracked_parser` (11), `test/test_cracked_manifest`
  (8), `test/test_cracked_sync` (7), `test/test_sync_scheduler` (5), `test/test_sync_session` (2), and the
  window-sharing cases in `test/test_upload_supervisor`, all through the fake fetcher/store/session and a
  fake clock.
- **Scenario J** (on-air, lane 3): **proven on a Cardputer against live wpa-sec.** The clock-advance
  stimulus (`huntLoopForceSyncDue`, behind `SAPPER_TEST_HOOKS`) forces a due sync without waiting an hour;
  observed `[SYNC] verify sync ok first=0 downloaded=26 new=0 malformed=0 overflow=0` — the TLS validated
  against the **pinned GTS Root R4**, the account downloaded and de-chunked to 26 results matching the
  probe, parsed with a zero malformed count, and the hunt resumed. `first=0`/`new=0` on a re-run proves
  the LittleFS manifest persisted and reloaded across runs and steady-state alerting is silent on an
  unchanged account. Artifact: `artifacts/0020-sync.txt`.
- **§4 standing invariant #12** was added in the same commit as ADR-0019: cracked passwords are read and
  written only through the `CrackedManifest`/`CrackedStore` seam. The slice-6 device surface
  (`SerialSyncObserver` in `hunt_loop.cpp`) consumes the sync's events and logs a crack **without** the
  plaintext password (essid, BSSID, and length only) — the PSK is a secret at rest (ADR-0006) and verify
  artifacts are committed as evidence (ADR-0004), so the plaintext stays in the manifest for slice-7's
  surfaces to read through the seam.
