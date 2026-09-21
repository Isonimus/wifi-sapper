---
id: '0060'
title: "Drop already-recovered captures at upload (gate A of ADR-0059)"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0059 gate A: the `UploadSupervisor` drops a queued capture whose BSSID is already in the
`CrackedManifest` instead of re-uploading it to wpa-sec, via a new read-only `CrackedLookup` seam.

- **`net/cracked_lookup.h` (pure interface):** `struct CrackedLookup { virtual bool isCracked(const
  uint8_t bssid[6]) const = 0; virtual ~CrackedLookup() = default; };`. Host-compiled.
- **`CrackedManifest` implements it:** `class CrackedManifest : public CrackedLookup`, with
  `bool isCracked(const uint8_t bssid[6]) const override { return find(bssid) != nullptr; }` — over the
  existing §4 #12 `find()` read path, no new store access.
- **`UploadSupervisor`:** an optional `const CrackedLookup* crackedLookup_ = nullptr` set by
  `void setCrackedLookup(const CrackedLookup&)` (mirrors `setNotifier`). In `drainQueue`, before the
  `read()` + `upload()` of each `pending[i]`, if `crackedLookup_ && crackedLookup_->isCracked(pending[i]
  .bssid)`: `queue_.remove(pending[i].id)`, log `[UPLOAD] skip bssid=… already recovered`, and continue —
  never calling `uploader_.upload` for it.
- **Wiring (`hunt_loop.cpp`):** `g_uploadSupervisor.setCrackedLookup(g_manifest);` after the manifest is
  begun — a device-only one-liner (review-only).

## Definition of Done

**Scenario A — a queued capture for an already-cracked BSSID is dropped, never uploaded.**
- **Given** a `CaptureQueue` (fake store) holding captures for BSSID X and BSSID Y, and a `CrackedLookup`
  that reports X cracked and Y not
- **When** the supervisor runs a drain cycle with the lookup set
- **Then** the uploader is called for Y only (never for X), X is `remove()`d from the queue, and X's
  removal is not counted as an upload error.
- **Proof:** `test/test_upload_supervisor` — fails before (X uploaded), passes after. See `## As built`.

**Scenario B — with no lookup set, behaviour is exactly as before (both uploaded).**
- **Given** the same queue but `setCrackedLookup` never called (`nullptr`)
- **When** a drain cycle runs
- **Then** both X and Y are uploaded — the slice-0018 drain path is untouched.
- **Proof:** `test/test_upload_supervisor` (the pre-existing drain tests still pass unchanged, plus an
  explicit no-lookup case). See `## As built`.

**Scenario C — the gate is manifest membership, not a guess.**
- **Given** a `CrackedManifest` (real type, fake store) seeded with one `CrackedResult` for BSSID X
- **When** `isCracked(X)` and `isCracked(Y)` are evaluated
- **Then** `isCracked(X)` is true and `isCracked(Y)` is false — i.e. `isCracked == (find != nullptr)`.
- **Proof:** `test/test_cracked_manifest` (the `CrackedLookup` override). See `## As built`.

## Verification

- **Gate + seam (Scenarios A, B, C)** — `pio test -e native` (`test_upload_supervisor`,
  `test_cracked_manifest`), fail-before/pass-after recorded in `## As built`. No new verify script: the
  behaviour is fully host-assertable (a fake queue + a fake/real lookup), unlike an on-air proof.
- **Device wiring (`hunt_loop` `setCrackedLookup`)** — review-only: a one-line injection of `g_manifest`,
  no new runtime path to drive headlessly.

## As built

Built as designed, with the narrow read-only seam ADR-0059 called for.

- **The seam (`net/cracked_lookup.h`).** A header-only pure abstract `struct CrackedLookup` with one
  method, `virtual bool isCracked(const uint8_t bssid[6]) const = 0`. No `.cpp`, so no build-filter
  change; it compiles into every lane that already sees the `net/` headers.
- **`CrackedManifest` implements it (Scenario C).** `class CrackedManifest : public CrackedLookup`, with
  `bool isCracked(const uint8_t bssid[6]) const override { return find(bssid) != nullptr; }` — over the
  existing §4 #12 `find()` read path, no new store access. `test/test_cracked_manifest` gains
  `test_is_cracked_is_manifest_membership`, which asserts `isCracked == (find != nullptr)` for a held and
  an unheld BSSID and ties the two together so they cannot silently diverge.
- **Gate A in the drain (`UploadSupervisor`, Scenarios A, B).** A new
  `void setCrackedLookup(const CrackedLookup&)` post-construction setter (mirroring `setNotifier`) stores
  an optional `const CrackedLookup* crackedLookup_ = nullptr`. In `drainQueue`, before the `read()` +
  `upload()` of each `pending[i]`, if the lookup is set and reports the BSSID cracked, the entry is
  `remove()`d and the loop continues — the uploader is never called for it. A successful remove is
  resolved like a purge (it does not fail the cycle); only a *failed* remove is a loud `storeErrors` that
  keeps the cycle dirty. The drop is counted in a new `DrainOutcome.skippedCracked`, surfaced on the bus
  in the `DrainCompleted` fact exactly as `purged`/`accepted` are (no Serial in this pure module).
- **Tests (`test/test_upload_supervisor`).** A trivial `FakeCrackedLookup` (a fixed cracked-set, no store)
  and a `pcapContainsBssid` helper that finds the AP's BSSID in the recorded upload bytes.
  `test_a_cracked_bssids_capture_is_dropped_never_uploaded` (Scenario A): X cracked + Y not → exactly one
  upload, and it is Y's capture (BSSID Y present, X absent), X counted `skippedCracked`, both gone from the
  queue, no store error. `test_with_no_lookup_every_capture_is_uploaded` (Scenario B): no lookup → both
  uploaded, nothing skipped. **Fail-before confirmed** by neutralising the drain gate (`if (false && …)`):
  Scenario A failed `Expected 1 Was 2` (X uploaded too) while Scenario B stayed green (it has no lookup, so
  it is behaviour-identical either way — the point of B); reverted, both pass. Full native suite green.
- **Wiring (`hunt_loop.cpp`, review-only).** `g_supervisor->setCrackedLookup(g_manifest);` right after the
  relay is aimed, once the manifest is `begin()`-loaded — the manifest (refreshed hourly by `CrackedSync`)
  is the live `CrackedLookup`. One device-only line; a physical drain cannot be host-driven.
- **§4.** Invariant #27 (added with ADR-0059) cites `test_upload_supervisor` + `test_cracked_manifest` for
  the gate + seam and review-only for the `hunt_loop` wiring.

Deviation from the slice plan: `DrainOutcome` gained a `skippedCracked` counter not spelled out in the
`## Goal`. The plan said the drop is "logged"; this module is pure and host-tested and does no Serial IO,
so the drop is surfaced the way every other drain outcome is — a counted field on the bus fact — rather
than a log line that would break the native lane. Same intent (the operator sees the dedup happen), the
mechanism the codebase already uses.

**Adversarial pass (Sonnet 5, blind to intent / aware of law).** Two real findings; concurrency,
lifetime, ordering, backoff accounting and §4 #12/#27 seam discipline all checked clean.
- **Rotated-PSK recovery is traded away (accepted limitation, recorded — operator decision).** Once a
  BSSID is recovered, `isCracked` is true for the entry's life, so gate A drops every later capture of
  it; if that network's PSK is rotated, the new handshake is never re-uploaded, wpa-sec never cracks the
  new key, and the `CrackedManifest` `Changed` path (ADR-0019 #5) can never fire — the surfaces report
  the stale old PSK until LRU eviction. Before gate A the endless re-uploads caught rotations. The
  operator chose to **accept and defer** (record + defer): gate A ships minimal, ADR-0059's edge note now
  states the limitation explicitly (correcting its earlier "no new failure mode"), and a LEDGER item
  carries the fully self-correcting fix (a bounded re-verification cadence). A recorded decision, not a
  silent gap (§2).
- **Failed skip-remove was untested (fixed).** The skip path's `queue_.remove()` failure branch
  (`++storeErrors; allTerminal = false;`) had no test. Added
  `test_a_failed_remove_of_a_cracked_capture_is_a_loud_store_error` (`failRemove` knob): a cracked entry
  whose remove fails is counted a store error, keeps the cycle dirty, is never uploaded, and stays queued
  for retry — fail-loud (§3), not silently counted clean.
- Rejected (informational): the reviewer noted the `SAPPER_TEST_HOOKS` upload probe wires the lookup
  unconditionally and could misreport if its fixed synthetic stimulus BSSID ever collided with a real
  manifest entry. A hardcoded test constant colliding with a live crack is too contrived to be a real
  risk; not acted on.
