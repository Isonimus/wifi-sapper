---
id: '0073'
title: "Bounded re-verification cadence for rotated PSKs"
type: slice
status: accepted
date: 2026-10-02
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0072: let the four rotated-PSK gates (A/B/C, ADR-0059/-0061/-0065) re-enable one known
network per re-verification interval so a rotated PSK is eventually re-captured, re-uploaded, re-cracked
by wpa-sec, and announced through the existing `Changed` alert. This closes the LEDGER item "bounded
re-verification cadence for rotated PSKs" — the accepted limitation all three gate ADRs deferred: today a
cracked or submitted BSSID is skipped permanently (until LRU eviction), so a key rotation is never
caught. A new persisted per-entry wall-clock `lastVerifiedEpoch` makes a stale entry targetable and
uploadable again; the fresh re-submission re-suppresses it. Fork D (ADR-0072 decision 3): the clock is a
manifest-cached epoch the device feeds, the lookup seams and `HuntEngine` are untouched, and the
freshness test is guarded so the pre-cadence behaviour holds verbatim before the first NTP sync.

## Definition of Done

- **Scenario A — a stale known network is re-enabled at both gate sites**
  - **Given** a cracked (or submitted) BSSID whose `lastVerifiedEpoch` is older than `kReverifyIntervalSec`
    against the manifest's cached verification clock
  - **When** the engine selects targets and the drain scans the queue
  - **Then** `isCracked`/`isSubmitted` report it **not** skipped (it is re-targeted and its queued capture
    is re-uploaded), whereas a BSSID verified within the interval is still skipped at both sites

- **Scenario B — a fresh re-submission re-suppresses the network (one re-verify per interval)**
  - **Given** a submitted BSSID that has gone stale and is being re-verified
  - **When** `markSubmitted` records its terminal re-upload
  - **Then** its `lastVerifiedEpoch` is bumped to the current verification clock (while `uploadedAt`, the
    eviction key, is unchanged), so it is skipped again until the next interval elapses

- **Scenario C — a re-crack bumps the cracked timer; a re-download of a known crack does not**
  - **Given** a held cracked BSSID
  - **When** `apply` processes a result for it — `New`/`Changed` (a real crack or rotated-PSK re-crack)
    versus `Unchanged` (the hourly re-download of the same PSK)
  - **Then** `New`/`Changed` stamp `lastVerifiedEpoch` fresh (re-suppressing it) and `Unchanged` leaves it
    untouched (so a known crack still eventually goes stale and is re-verified — the ADR-0072 decision-4
    correctness point)

- **Scenario D — the cadence is inert without a real clock (pre-NTP / offline), and never spins**
  - **Given** a manifest whose cached verification clock is below `kMinRealEpoch` (a device that has not
    yet completed an NTP sync)
  - **When** the gates are consulted
  - **Then** every member is skipped exactly as before the cadence existed (no entry is ever stale); and
    **given** every discovered/queued entry is stale, selection and the drain still fall through to a
    fresh discovery sweep / an empty drain, never a spin

## Design

Grounded in the measured clock survey (ADR-0072 Context) and the current manifests
(cracked_manifest.cpp, submitted_manifest.cpp), the drain (upload_supervisor.cpp), the sync
(cracked_sync.cpp), and the two LittleFS stores. What was considered, reused, and not reused:

- **Clock location — manifest-cached, device-fed (Fork D), not threaded into the gates (Fork A).** Reused
  the existing fake-clock discipline (a plain integer "now" injected into pure code, never read from
  `millis()`/`time()` internally) but as a cached member set by `setVerificationClock`, not a new
  parameter on `isCracked`/`isSubmitted`. This keeps the lookup seams and the engine untouched — the
  engine gains no epoch parameter and no second clock domain — so §4 #8/#10 hold with nothing new on the
  frame path. The clock is read only where `bringUpStation()` already guarantees valid epoch (the device
  loop feeding `std::time(nullptr)`, inert under the `kMinRealEpoch` guard until the first NTP sync).
- **Timer on both manifests, not submitted-only** (ADR-0072 decision 5): the submitted set is not durable
  (flash wipe), so a post-wipe re-seeded crack needs the cracked entry's own timer. Rejected the
  single-timer simplification for that reason.
- **Freshness helper — one pure function, `reverifyDue(lastVerifiedEpoch, nowEpoch)` in a new
  `net/reverification.h`** beside the two policy constants (`kReverifyIntervalSec`, `kMinRealEpoch`). It
  carries the subtle part (the valid-epoch guard and a backwards-clock-safe age) in one tested place
  rather than duplicated across the two manifests. `kMinRealEpoch` deliberately duplicates
  wifi_station.cpp's device-only TLS threshold (rule of three — second instance; see ADR-0072
  consequences), not hoisted here.
- **Persisted format — magic bump + back-compatible load** (ADR-0072 decision 7): reused the stores'
  existing versioned-magic + field-by-field layout; added a four-byte `lastVerifiedEpoch` per record,
  bumped `CRK1→CRK2` / `SUB1→SUB2`, and taught the loader to read a v1 record at its old length with the
  field defaulted to 0. An unknown magic stays a loud failure. Did **not** reinterpret by file size.
- **Supervisor untouched.** Because freshness is internal to the manifests, gates A/B/C lift
  automatically; the only wiring is `hunt_loop` feeding the clock (review-only, like the existing
  `setCrackedLookup`/`setSubmittedLookup` wiring).

Mechanism: `hunt_loop` calls `setVerificationClock(std::time(nullptr))` on both manifests each loop.
`isCracked`/`isSubmitted` return `member && !reverifyDue(entry.lastVerifiedEpoch, nowEpoch_)`.
`markSubmitted` stamps `lastVerifiedEpoch = nowEpoch_` on both a new and a held BSSID (keeping
`uploadedAt`); `apply` stamps it on `New`/`Changed`, never on `Unchanged`.

## Verification

Host-assertable in full at the struct/policy level (the cadence is pure state-machine behaviour over the
fake stores and an injected epoch — no RF/rendering/real timing), so there is no verify script (ADR-0004:
one ships only when a unit test cannot assert the behaviour). The LittleFS **byte-format migration** is
device-only (`#ifndef UNIT_TEST`, like all existing store byte-format code — the fakes round-trip structs,
not bytes) and stays review-only, proven on air by the existing sync/maintenance verifies; the
struct-level `lastVerifiedEpoch` round-trip and the 0-default semantics it relies on are host-tested via
the fakes. New regressions:

- Scenario A — `test/test_cracked_manifest` and `test/test_submitted_manifest`: a member verified within
  the interval is reported cracked/submitted; one past the interval is not.
- Scenario B — `test/test_submitted_manifest`: `markSubmitted` on a held BSSID bumps `lastVerifiedEpoch`
  and keeps `uploadedAt`.
- Scenario C — `test/test_cracked_manifest`: `apply(New)`/`apply(Changed)` stamp the timer,
  `apply(Unchanged)` does not (so the entry still goes stale on schedule).
- Scenario D — both manifest suites: a sub-`kMinRealEpoch` clock reports every member still cracked/
  submitted (pre-cadence behaviour). The all-stale no-spin half needs **no new** engine/supervisor test:
  those gate sites are unchanged, and a stale entry makes `isCracked`/`isSubmitted` answer `false` — the
  exact not-cracked/not-submitted case the existing all-cracked/all-submitted fall-through tests (§4
  #28/#30) already cover. Adding one would be a duplicate test (quality bar §3).

Fail-before is shown by neutralising the freshness test (a stale member keeps reporting skipped →
Scenario A fails) and the `markSubmitted` bump (a re-verified network never re-suppresses → Scenario B
fails); recorded in `## As built`.

## As built

Shipped as designed (Fork D). A new `src/net/reverification.h` holds the policy: `kReverifyIntervalSec`
(7 days), `kMinRealEpoch` (2020-01-01, the valid-clock guard — a deliberate second instance of
wifi_station.cpp's TLS threshold, rule of three), and the pure `reverifyDue(lastVerifiedEpoch, nowEpoch)`
test (guarded below `kMinRealEpoch`, backwards-clock-safe age). `CrackedEntry`/`SubmittedEntry` each gained
a persisted `uint32_t lastVerifiedEpoch`; `CrackedManifest`/`SubmittedManifest` each gained
`setVerificationClock(nowEpoch)` and a cached `nowEpoch_`, made `isCracked`/`isSubmitted`
re-verification-aware (keeping their signatures), stamped the timer in `markSubmitted` (both a new and a
held BSSID, keeping `uploadedAt`) and in `apply(New|Changed)` (never `Unchanged`). The two LittleFS stores
bumped magic (`CRK1→CRK2` / `SUB1→SUB2`), write the four-byte field, and still load the old magic
(defaulting the field to 0). `hunt_loop` feeds `setVerificationClock(std::time(nullptr))` on both manifests
each loop, before the engine/supervisor tick. The `UploadSupervisor` and `HuntEngine` are untouched — the
freshness is internal to the manifests. §4 row #33 added. LEDGER: the re-verification item is closed; an
interval-tuning item added.

Verification — all four Definition-of-Done scenarios met, as six new regressions (326/326 native, was 320;
cardputer + cardputer_testhooks both build):

- Scenario A → `test_stale_cracked_entry_is_reverification_eligible` + `test_stale_submitted_entry_is_reverification_eligible`
- Scenario B → `test_remark_bumps_the_reverification_timer_keeping_uploadedAt`
- Scenario C → `test_apply_stamps_the_timer_except_on_unchanged`
- Scenario D → `test_cracked_cadence_is_inert_without_a_real_clock` + `test_cadence_is_inert_without_a_real_clock`
  (the all-stale no-spin half needs no new engine/supervisor test — a stale entry is the already-covered
  `isCracked`/`isSubmitted == false` case at those unchanged sites)

Fail-before shown with two coherent, isolated mutations: forcing `reverifyDue` to return false (freshness
test disabled) failed Scenario A in both suites while the guard and stamp tests stayed green; removing the
held-path re-stamp in `markSubmitted` failed only `test_remark_bumps_...` while Scenario A and the
idempotent-`uploadedAt` test stayed green. Both reverted.

Adversarial pass (blind to intent, aware of law; Sonnet 5; read the files itself, no recursion) — required
because this adds §4 invariant #33, a persisted on-disk format change, and new public manifest API. It
attacked the epoch arithmetic (unsigned wrap, backwards clock, the `kMinRealEpoch` guard, the interval
boundary), both stores' migration (version branching, the shared read buffer, short-record/unknown-magic
fail-loud), both stamp sites (`apply` New/Changed/Unchanged incl. the eviction slot; `markSubmitted` held
vs new, `uploadedAt` untouched), the §4 #8/#10 concurrency claim (grepping every call site — all app-task,
none on `onFrame`), the clock-feed ordering, and convergence/no-spin; and ran the suite itself (326/326).
**No findings.** Its one non-blocking note — that a never-rotating PSK's cracked-gate entry stays stale and
suppression is carried by the submitted timer's restamp — is exactly the convergence ADR-0072 decision 5
documents, not a defect. No change resulted.
