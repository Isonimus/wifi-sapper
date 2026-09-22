---
id: '0065'
title: "Skip already-submitted networks in the hunt (gate C, the general form of gates A/B)"
type: architecture
status: accepted
date: 2026-09-22
supersedes: []
superseded_by: []
---

## Context

Gates A (ADR-0059, upload-side drop) and B (ADR-0061, targeting-side skip) both key on the **cracked**
manifest: a network wpa-sec has already recovered needs neither re-uploading nor re-targeting. But
"cracked" is the *end* of the pipeline, and the redundant work starts the moment a network is
**submitted** — the instant wpa-sec has accepted a valid handshake for it. Between submission and a
crack (which may be hours, days, or never, if the PSK is not in wpa-sec's wordlist), the endless hunt
keeps re-targeting that BSSID, sitting on it for a full `captureWindowMs`, deauthing it when armed
(ADR-0029), and re-uploading each fresh capture — none of which changes anything wpa-sec does. A
second, third, Nth handshake for the same BSSID does not crack it any faster: wpa-sec already holds a
valid handshake and is trying its wordlist against that PSK; more handshakes of the *same* PSK add no
cracking power (a distinct measured fact — because ANonce/SNonce are random per 4-way handshake, each
fresh capture is a genuinely *different* 22000 hash wpa-sec will *Accept* rather than dedup, so the
redundant uploads are not even suppressed by wpa-sec's own duplicate detection; they are pure waste).

So the general rule the cracked gates approximate is: **skip a network we have already landed a valid
handshake for at wpa-sec.** Cracked is a strict subset of submitted (we can only recover what we, or
another account, submitted), so gate C's skip *contains* gates A/B's — but it does not replace them,
for a robustness reason developed in decision #4 below.

The mechanics mirror gates A/B exactly. `HuntEngine::doStartCapturing` already walks targets in one
`while` loop on the **app task**, skipping any it should not capture (`++targetIndex_; continue;`); the
`UploadSupervisor::drainQueue` already drops a queued capture it should not upload
(`queue_.remove(...)`), keying on the read-only `CrackedLookup` seam. Gate C adds the same two skips
keyed on a new **submitted** set. What is genuinely new is the *state*: unlike the cracked manifest,
which is **downloaded** from wpa-sec and rewritten whole on each sync, the submitted set is a **local**
fact — "*this* appliance uploaded a valid handshake for this BSSID" — recorded on the upload path. It
therefore needs its own persistence, its own writer, and its own read seam.

## Decision

1. **A separate `SubmittedStore` + `SubmittedManifest`, not an extension of the cracked manifest.** The
   submitted set persists through its own seam (`net/submitted_store.h`, LittleFS-backed on device at
   `/submitted.bin`, in-RAM fake on the host lane), mirrored by a pure bounded policy
   (`net/submitted_manifest.h`) exactly as `CrackedStore`/`CrackedManifest` are paired. A
   `SubmittedEntry` is only `{bssid[6], uploadedAt}` — no ESSID, no PSK (a submitted-but-not-cracked
   network has none). The bound is `kMaxSubmittedEntries = 256`, oldest-`uploadedAt`-evicted at the
   bound, like the cracked manifest's oldest-crack eviction (ADR-0019 decision #4). Folding "submitted"
   into the cracked manifest was rejected: (a) the cracked manifest is *rewritten whole* on every sync
   `flush()` from the downloaded results, so a submitted-not-cracked entry would be wiped on the next
   sync — or force the upload path to read-merge-rewrite the entire cracked file; (b) it would put a
   **second writer** (the upload drain) on the cracked store, breaking the single-writer discipline §4
   #12 protects; (c) the two facts have different lifecycles (downloaded-and-refreshed vs.
   locally-appended) and different robustness (see #4). Rule-of-three (quality bar §3) also says do
   *not* extract a shared "bounded per-BSSID map" abstraction yet — there are exactly two instances and
   they differ (payload vs. none), so a premature generalisation would be speculative.

2. **Two narrow seams, mirroring the cracked ones.** A read-only `SubmittedLookup { isSubmitted(bssid) }`
   (`net/submitted_lookup.h`) is what the *engine* consumes — the exact analogue of `CrackedLookup`, so
   the engine couples to the *question* and stays host-testable against a trivial fake. The
   *supervisor* both reads and writes, so it consumes a `SubmittedRecorder : SubmittedLookup` that adds
   `markSubmitted(bssid, nowMs)` and `flushSubmitted()`. `SubmittedManifest` implements
   `SubmittedRecorder`. Both consumers take the seam through an **off-by-default setter**
   (`HuntEngine::setSubmittedLookup`, `UploadSupervisor::setSubmittedRecorder`): unset → the exact
   pre-gate-C behaviour, so §4 #8/#10 (engine) and the slice-0018 drain (supervisor) are the baseline
   and gate C is the addition, matching gates A/B and the `RawTransmitter` arm.

3. **Written only on a terminal success, in the drain.** `UploadSupervisor::drainQueue` calls
   `markSubmitted(bssid, nowMs)` when — and only when — an upload returns `UploadResult::Accepted` **or**
   `UploadResult::Duplicate`. Both mean wpa-sec has taken a *valid* handshake for that BSSID; a
   `Rejected` upload did **not** land, so it is never marked (gate on knowledge, not hope — identical to
   ADR-0059 decision #4 / ADR-0061 decision #5). Marks accumulate in RAM across the drain loop and are
   persisted by **one** `flushSubmitted()` at the end of the cycle, so a whole drain is one submitted-store
   write, matching the cracked manifest's one-write-per-sync discipline. A `flushSubmitted()` failure is
   surfaced loud (a counted `submittedStoreErrors`) but does **not** fail the upload cycle or drive its
   backoff: the uploads themselves already succeeded and their queue entries are already removed, so
   there is nothing to retry — a lost submitted record only means a future cycle may re-target/re-upload
   that BSSID (a degradation, never a lost handshake). This mirrors how a sync outage is deliberately
   kept out of the upload backoff (ADR-0019 decision #7).

4. **Gate C does not supersede gates A/B; it complements them, with cracked as the durable floor.**
   Applied at the two sites: `HuntEngine::doStartCapturing` skips a target that is cracked **or**
   submitted; `drainQueue` drops a queued capture that is cracked **or** submitted. Although cracked ⊆
   submitted *normally*, the two sets have different robustness under the appliance's routine power loss
   and flash wear: the **cracked** set is re-seeded from wpa-sec on every sync, so it survives a flash
   wipe; the **submitted** set is purely local, so a wipe (or a `SubmittedManifest::begin()` load
   failure, or eviction past the 256 bound) empties it. Were gate C to *replace* the cracked gates, a
   flash loss would silently re-enable re-targeting and re-uploading of already-cracked networks until
   the next sync re-seeded the cracked set. Keeping gates A/B as the floor means the cracked skip always
   holds; gate C adds the submitted skip on top for the not-yet-cracked window. §4 #27 and #28 keep
   their status unchanged.

5. **Same task as the writer — no lock.** `isSubmitted` is read on the app task (engine target
   selection, and the drain, which also runs on the main loop task); `markSubmitted`/`flushSubmitted`
   are written on the same app task inside the drain cycle. The driver-task RX callback never touches
   the submitted manifest. So the reads are race-free without a lock, exactly like the cracked gates
   (ADR-0061 decision #4). The engine's `isSubmitted` read is in the app-task selection loop, never in
   `onFrame`, so §4 #8/#10 are preserved by construction as with gate B.

## Consequences

- The appliance stops re-targeting, re-deauthing, and re-uploading a network from the moment it has a
  valid handshake at wpa-sec — not merely from the moment it is cracked. On a busy site this is the
  dominant saving: a network typically sits *submitted-but-uncracked* far longer than it sits *cracked*
  (many PSKs are never in wpa-sec's wordlist at all), and gate C reclaims that whole window of otherwise
  wasted capture airtime, deauth bursts, and redundant uploads.
- **New §4 invariant #30**, added in this commit: the hunt skips an already-submitted BSSID in target
  selection and the drain drops an already-submitted queued capture, via the new `SubmittedLookup`/
  `SubmittedRecorder` seams over a `SubmittedManifest`; the submitted set is written only on a terminal
  Accepted/Duplicate, and gate C complements #27/#28 (cracked floor) without superseding them.
- `HuntEngine` gains one more optional read-only pointer and setter; `UploadSupervisor` gains one
  optional recorder pointer, one setter, one `markSubmitted` call per terminal success, and one
  `flushSubmitted` per cycle. The frame path, the pure capture core, and the RX callback are untouched.
- The discovered-AP *count* (and the HUD/registry) still includes submitted APs — like cracked ones,
  they are enumerated at discovery and only removed from the *capture* round-robin, keeping the RF
  picture honest (ADR-0061 consequence).
- **`uploadedAt` is a `millis()` value**, meaningful only for relative oldest-eviction *within one boot*
  (it resets across reboots, exactly like the cracked manifest's `firstSeenCrackedMs` and for the same
  reason — no persisted wall clock; ADR-0019). It is deliberately **not** a re-verification timer. A
  bounded re-verification / re-submission cadence — re-hunting a submitted (or cracked) network after N
  days in case its PSK rotated — needs NTP-backed wall-clock time and stays deferred (LEDGER, the same
  item ADR-0059/-0061 defer their rotation edge to; gate C compounds it further, since a submitted
  network is now skipped even before it is cracked).

## Alternatives considered

- **Extend `CrackedManifest` into a wpa-sec *status* store** (`NOT_UPLOADED`/`UPLOADED`/`CRACKED`, the
  Adversary's one-file model). Rejected for the three reasons in decision #1: the sync's whole-rewrite
  would wipe submitted-not-cracked entries, it puts a second writer on the cracked store (§4 #12), and
  it conflates a downloaded fact with a local one whose robustness must differ (decision #4).
- **Make gate C's submitted set the single filter, superseding gates A/B.** Rejected in decision #4:
  the submitted set is local and lossy, so it cannot be the durable floor; the cracked set, re-seeded
  from wpa-sec, must remain authoritative.
- **Targeting-only (no queue drop).** Considered and rejected: the queue drop is ~5 lines at the site
  gate A already occupies, and it catches captures stranded on flash from before the target-skip took
  effect or from a prior boot — completing the "general form of gates A/B" the drain side of gate A
  established. The operator confirmed targeting + queue.
- **Mark submitted on enqueue rather than on upload success.** Rejected: an enqueued capture has not
  reached wpa-sec (the STA window may fail, the upload may be `Rejected`), so marking it submitted would
  gate on hope and could permanently skip a network whose handshake never actually landed — the exact
  failure decisions #3 and gate A's "knowledge not hope" exist to prevent.
