---
id: '0066'
title: "Skip already-submitted networks in the hunt"
type: slice
status: accepted
date: 2026-09-22
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0065 gate C: skip a network the moment this appliance has landed a *valid handshake* for
it at wpa-sec (terminal `Accepted`/`Duplicate`), not merely once it is cracked. The skip applies at both
sites gates A/B occupy — `HuntEngine` target selection and the `UploadSupervisor` drain — keyed on a new
local **submitted** set, and complements the cracked gates (#27/#28) without replacing them (cracked
stays the durable, re-seeded floor).

- **`SubmittedStore` (`net/submitted_store.h`):** the persistence seam under the submitted set, mirroring
  `CrackedStore`. `SubmittedEntry` = `{bssid[6], uploadedAt}`. `load`/`save`, fail-loud. LittleFS backing
  (`net/submitted_store_littlefs.{h,cpp}`, device-only, `/submitted.bin`, temp-then-rename); in-RAM host
  fake (`test/support/fake_submitted_store.h`).
- **`SubmittedLookup` / `SubmittedRecorder` (`net/submitted_lookup.h`):** the read seam
  (`isSubmitted(bssid)`, consumed by the engine) and its write-capable extension
  (`markSubmitted(bssid, nowMs)` + `flushSubmitted()`, consumed by the supervisor). Mirror `CrackedLookup`.
- **`SubmittedManifest` (`net/submitted_manifest.{h,cpp}`):** the pure bounded policy implementing
  `SubmittedRecorder` over a `SubmittedStore` — `kMaxSubmittedEntries = 256`, oldest-`uploadedAt`
  eviction at the bound, no fresh flag (submitted has no first-sync semantics).
- **`HuntEngine`:** `setSubmittedLookup(const SubmittedLookup&)` (off by default); in `doStartCapturing`,
  after the gate-B cracked check, skip a target whose `isSubmitted` is true (`++targetIndex_; continue;`).
- **`UploadSupervisor`:** `setSubmittedRecorder(SubmittedRecorder&)` (off by default); in `drainQueue`,
  after the gate-A cracked drop, drop a queued capture whose `isSubmitted` is true (counted
  `skippedSubmitted`); on a terminal `Accepted`/`Duplicate`, `markSubmitted(bssid, nowMs)`; once per
  cycle, `flushSubmitted()` (a failure counted `submittedStoreErrors`, does not fail the upload cycle).
- **Wiring (`hunt_loop.cpp`):** construct the LittleFS store + manifest, `begin()` (fatal on load
  failure), and wire the manifest into both consumers.
- **§4:** add invariant #30 (submitted-network skip at both sites) in the same commit as ADR-0065.

## Definition of Done

**Scenario A — an already-submitted AP is never targeted; a not-submitted one is.**
- **Given** discovery enumerates AP X (marked submitted) and AP Y (not submitted), gate C wired
- **When** the engine runs its discover→capture cycle and both APs' EAPOL is available
- **Then** the engine captures Y (raises `onCaptureReady` for Y) and **never enters Capturing for X**
  (X is never `capturingBssid()`, so it is never tuned to or deauthed) — the round-robin skips X.
- **Proof:** `test/test_hunt_engine` — fails before (X is captured too), passes after. See `## As built`.

**Scenario B — no submitted lookup wired degrades to the pre-gate-C engine.**
- **Given** `setSubmittedLookup` is never called (nullptr)
- **When** the engine runs its cycle with two discovered APs
- **Then** both are targeted and captured — no submitted state is consulted (there is none), §4 #8/#10
  preserved. All discovered submitted → re-discovery, never a spin (the all-cracked fall-through).
- **Proof:** `test/test_hunt_engine` — unchanged from the no-lookup baseline. See `## As built`.

**Scenario C — the drain drops an already-submitted queued capture and marks new submissions.**
- **Given** a queue holding a capture for submitted AP X and one for not-submitted AP Y, gate C wired
- **When** one drain cycle runs and Y uploads `Accepted`
- **Then** X is removed without upload (counted `skippedSubmitted`), Y is uploaded and its BSSID is
  `markSubmitted` (now `isSubmitted(Y)`), and exactly one `flushSubmitted()` persists the cycle.
- **Proof:** `test/test_upload_supervisor` — fails before (X uploaded too / Y not marked). See `## As built`.

**Scenario D — only a terminal success marks submitted; a rejection does not.**
- **Given** a queue holding a capture for AP Z, gate C wired, the uploader scripted to `Rejected`
- **When** one drain cycle runs
- **Then** Z is **not** `markSubmitted` (it stays targetable/uploadable — gate on knowledge, not hope),
  and a subsequent `Duplicate` on a retry **does** mark it. A `flushSubmitted()` failure is counted
  `submittedStoreErrors` and does **not** fail the cycle (uploads already succeeded).
- **Proof:** `test/test_upload_supervisor`. See `## As built`.

**Scenario E — `SubmittedManifest` round-trips, bounds, and evicts the oldest.**
- **Given** a `SubmittedManifest` over a fake store
- **When** BSSIDs are marked (including past the bound) and `flushSubmitted`/`begin` round-trip through
  the store
- **Then** `isSubmitted` is true exactly for marked-and-retained BSSIDs, re-marking is idempotent
  (membership, original `uploadedAt` kept), the count never exceeds the bound, the oldest `uploadedAt`
  is evicted first, and a store `load`/`save` failure is surfaced (fail-loud).
- **Proof:** `test/test_submitted_manifest`. See `## As built`.

## Verification

- **Both skip sites + write trigger + manifest policy (Scenarios A–E)** — `pio test -e native`
  (`test_hunt_engine`, `test_upload_supervisor`, `test_submitted_manifest`), fail-before/pass-after
  recorded in `## As built`. No verify script: the behaviour is fully host-assertable (fake sniffer + real
  engine, fake store + real manifest + real queue) — like the cracked gates, unlike an on-air proof. The
  device-only LittleFS backing (`submitted_store_littlefs.cpp`) is the same temp-then-rename pattern the
  cracked store's on-air paths already exercise; it carries no new on-air behaviour to prove.

## As built

Shipped as designed. A new `SubmittedStore` seam (`net/submitted_store.h`, `SubmittedEntry =
{bssid[6], uploadedAt}`) with a LittleFS backing (`net/submitted_store_littlefs.{h,cpp}`, device-only,
`/submitted.bin`, magic `SUB1` + u16 count + 10-byte records, temp-then-rename atomicity — the exact
idiom `LittleFsCrackedStore` uses) and an in-RAM host fake (`test/support/fake_submitted_store.h`); two
narrow seams (`net/submitted_lookup.h` — `SubmittedLookup::isSubmitted` and its write-capable
`SubmittedRecorder : SubmittedLookup` with `markSubmitted`/`flushSubmitted`); and the pure bounded
policy `SubmittedManifest` (`net/submitted_manifest.{h,cpp}`, `kMaxSubmittedEntries = 256`,
oldest-`uploadedAt` eviction, idempotent re-mark keeping the original stamp, no fresh flag).
`HuntEngine::doStartCapturing` skips an `isSubmitted` target with `++targetIndex_; continue;` after the
gate-B cracked check; `UploadSupervisor::drainQueue` (now taking `nowMs`) drops an `isSubmitted` queued
capture (`skippedSubmitted`) after the gate-A cracked drop, `markSubmitted`s each terminal
`Accepted`/`Duplicate`, and `flushSubmitted`s once per cycle (a failure counted `submittedStoreErrors`,
kept out of the cycle-success expression). Both seams are off by default; `hunt_loop` mounts the store,
loads it (fatal on load failure), and wires the manifest into both consumers.

- **Scenarios A, B — `test/test_hunt_engine`** (`test_submitted_ap_is_never_targeted_but_unsubmitted_one_is`,
  `test_submitted_lookup_wired_but_empty_targets_every_ap`, `test_all_submitted_degrades_to_rediscovery_not_a_spin`),
  driven through the real state machine via the fake sniffer feeding a real `ApRegistry` through the real
  `onFrame` seam.
- **Scenarios C, D — `test/test_upload_supervisor`** (`test_c_submitted_capture_is_dropped_and_new_submission_is_recorded`,
  `test_c_no_recorder_wired_uploads_and_records_nothing`, `test_d_only_a_terminal_success_records_submitted`,
  `test_d_flush_failure_is_loud_but_not_a_cycle_failure`), driving a real `SubmittedManifest` over a
  `FakeSubmittedStore` through the real drain.
- **Scenario E — `test/test_submitted_manifest`** (membership, idempotent re-mark keeping the original
  `uploadedAt`, whole round-trip through the store, bound + oldest eviction, fail-loud load/save).
- **Fail-before/pass-after:** neutralising the `doStartCapturing` gate-C skip (`if (false && …)`) failed
  the two targeting tests (`Expected TRUE Was FALSE` — the submitted AP was captured; `Expected 1 Was 2`
  — nothing skipped, so the registry was not re-swept) while the wired-but-empty case stayed green;
  neutralising the drain's gate-C drop + mark failed the three writing/dropping tests (`skippedSubmitted`
  0 not 1; `isSubmitted` false after a terminal Duplicate; `submittedStoreErrors` 0 not 1) while the
  no-recorder baseline stayed green. Restored → the full **300-case** native suite passes (287 before +
  6 `test_submitted_manifest` + 3 `test_hunt_engine` + 4 `test_upload_supervisor`); `cardputer` firmware
  builds (41.2% flash); lint 0/0 (66 documents); INDEX byte-identical to the generator.
- **Adversarial pass (Sonnet 5, blind to intent, aware of law — diff + CLAUDE.md + INDEX.md, read the
  files itself, no recursion):** no defects (critical/major/minor). It independently confirmed the binary
  round-trip against `LittleFsCrackedStore` (magic, LE count/timestamp, 10-byte record, overrun guard,
  rename-over-target power-loss safety, fail-loud paths), the manifest's idempotent re-mark and
  oldest-eviction and hard-`false` load-failure, the drain's gate ordering (cracked-then-submitted, so a
  both-cracked-and-submitted entry is counted once under `skippedCracked`), the Accepted/Duplicate-only
  write with a single `flushSubmitted` per cycle whose failure is excluded from the success expression,
  the single `nowMs` threaded to every mark, the app-task-only `isSubmitted` read (§4 #8/#10 hold), the
  §4 #30 row's accuracy (and that it does not supersede #27/#28, which are untouched), and the build
  wiring (`submitted_manifest.cpp` on the native allow-list, the `#ifndef UNIT_TEST` LittleFS backing
  compiling on device by default) — mirroring exactly how the cracked store is wired. Its one cosmetic
  note (the `SubmittedEntry` RAM struct pads to ~12 bytes, not the 10-byte on-disk record) was applied to
  the `kMaxSubmittedEntries` comment.
