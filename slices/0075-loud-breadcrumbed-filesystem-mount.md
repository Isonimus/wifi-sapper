---
id: '0075'
title: "Loud, breadcrumbed LittleFS mount seam"
type: slice
status: accepted
date: 2026-10-25
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0074: replace the four scattered `LittleFS.begin(formatOnFail=true)` calls with one
`ensureFilesystemMounted()` seam that tries a non-formatting mount first and, on failure, classifies the
result. A reformat of a partition that previously held a filesystem is loud (one `[ERROR]`) and
breadcrumbed (a durable NVS reformat count); a blank or erased partition's first-ever format is quiet; a
dead filesystem (even the reformat fails) halts via the caller's `[FATAL]`. Surface the breadcrumb on the
Maintenance dashboard. This closes the silent data-destruction path a field run exposed (a mount failure
was silently reformatted), and unblocks the on-air CRK1 to CRK2 migration proof: once a corrupt mount is
loud and never silently eats a store, a deliberately re-seeded old-format store can be migrated and
verified for real.

## Definition of Done

- **Scenario A: a clean mount is silent and reports Mounted**
  - **Given** a filesystem that mounts without formatting
  - **When** `ensureFilesystemMounted()` runs
  - **Then** it returns `Mounted`, logs no `[ERROR]`, and does not touch the breadcrumb counter

- **Scenario B: a reformat of a previously-seen filesystem is loud, breadcrumbed, and recovers**
  - **Given** a partition that was mounted before (the `fs_seen` marker is set) whose non-formatting
    mount now fails but whose formatting mount succeeds
  - **When** `ensureFilesystemMounted()` runs
  - **Then** the pure `decideMountOutcome(false, true, true)` returns `Reformatted` (distinct from both
    `Mounted` and `FirstFormat`), which drives one loud `[ERROR]` and one increment of the persisted
    `reformats` breadcrumb, and the store continues (its `begin()` returns success)

- **Scenario C: a blank partition's first-ever format is quiet, not a data-loss reformat**
  - **Given** a partition that was never mounted before (the `fs_seen` marker is unset), e.g. a fresh or
    erased device, whose non-formatting mount fails and whose formatting mount succeeds
  - **When** `ensureFilesystemMounted()` runs
  - **Then** `decideMountOutcome(false, true, false)` returns `FirstFormat` (not `Reformatted`), so there
    is no `[ERROR]` (it must not trip the verify gates on first boot) and the breadcrumb stays untouched

- **Scenario D: a dead filesystem fails loud and halts**
  - **Given** a filesystem whose formatting mount also fails
  - **When** `ensureFilesystemMounted()` runs
  - **Then** `decideMountOutcome(false, false, *)` returns `Failed`, the store `begin()` returns `false`,
    and the caller's existing `[FATAL]` fires (boot halts, as today)

- **Scenario E: the breadcrumb is durable and shown**
  - **Given** a unit whose `reformats` count is non-zero in NVS
  - **When** the Maintenance dashboard is rendered
  - **Then** it reports the filesystem was reformatted N times (persisted data lost), and a zero count
    renders the healthy line

## Design

Grounded in the measured field log (ADR-0074 Context), the four mount sites
(`capture_store_littlefs.cpp`, `cracked_store_littlefs.cpp`, `submitted_store_littlefs.cpp`,
`rssi_probe.cpp`), the NVS `Preferences` pattern (`provisioning_store.cpp`), and the pure dashboard
renderer (`net/dashboard`). Considered, reused, not reused:

- **One seam, `net/filesystem_mount`.** The pure `decideMountOutcome(firstOk, reformatOk,
  hadFilesystemBefore)` lives in the header (host-testable); the device-only `.cpp` holds
  `ensureFilesystemMounted()`, the `fs_seen` marker helpers, and the breadcrumb. Reused the
  host-tested-core / device-only-glue split every store uses. The rule-of-three hoist of four duplicated
  `LittleFS.begin(formatOnFail=true)` sites.
- **A durable `fs_seen` marker distinguishes first-format from reformat.** A blank partition reports the
  identical mount failure as a corrupt one, so without it a fresh device would false-alarm. The marker and
  the breadcrumb live in a dedicated `fshealth` NVS namespace (not `sapper`), so a provisioning `clear()`
  never wipes them and a LittleFS format cannot (NVS is a separate partition). The marker write fails loud
  like `recordReformat()` and `provisioning_store`, so a broken marker is diagnosable, never a silent
  reclassification of future reformats.
- **Breadcrumb is a `uint32` counter, not a timestamp:** the mount runs pre-NTP, so an epoch would be a
  misleading 1970.
- **Routed the rssi probe through the seam too**, so `LittleFS.begin` appears in exactly one file and §4
  #34 is a grep.
- **Dashboard line via the existing pure `net/dashboard` renderer**, reading the count through a
  `DashboardStats` field like the Maintenance dashboard already reads persisted state (no new engine
  reach, §4 #13/#21).
- **Did not** add a per-script verify-gate change (the loud `[ERROR]` rides the existing gate); broader
  ESP-IDF-error hardening is a separate LEDGER item. **Did not** record a per-boot reset reason
  (speculative LEDGER contingency, since the original failure was an unformatted partition, not
  corruption).

## Verification

Host-assertable at the policy level, so no verify script (ADR-0004): the branching the fix turns on is
the pure `decideMountOutcome`, and the dashboard line is a pure render over the count. The device glue
(`LittleFS.begin`, the NVS marker and breadcrumb) is device-only like all store/NVS byte code and stays
review-only, proven on the on-air sync/maintenance run (which is also the CRK1 to CRK2 migration proof,
LEDGER).

- Scenarios A/B/C/D: `test/test_filesystem_mount` on `decideMountOutcome` (all four outcomes, with
  `Reformatted` asserted distinct from both `Mounted` and `FirstFormat`, and `FirstFormat` distinct from
  `Reformatted`: the two distinctions the silent path and the false-alarm bug each lost).
- Scenario E: `test/test_dashboard` renders the reformat-count line for a non-zero count and the healthy
  line for zero.

Fail-before shown by collapsing the first-format branch of `decideMountOutcome` back into `Reformatted`
(the false-alarm behaviour): Scenario C's distinctness assertion fails while the others stay green.
Recorded in `## As built`.

## As built

Shipped as designed. `net/filesystem_mount` holds the pure `decideMountOutcome(firstMountOk,
reformatMountOk, hadFilesystemBefore)` returning `Mounted` / `FirstFormat` / `Reformatted` / `Failed`, and
the device-only `ensureFilesystemMounted()` + `filesystemReformatCount()` plus file-local `fs_seen`
helpers. `ensureFilesystemMounted` tries a non-formatting `LittleFS.begin`; on success it marks the
filesystem seen and returns `Mounted`. On failure it reads the pre-existing `fs_seen` marker, formats, and
either logs a loud `[ERROR]` + increments the breadcrumb (`Reformatted`, a previously-seen partition) or
logs a benign `[FS]` line (`FirstFormat`, a blank partition); `Failed` lets the caller's `[FATAL]` halt.
The three stores and the rssi probe route through the seam, so `LittleFS.begin` is in exactly one file.
The Maintenance dashboard gained `DashboardStats.fsReformatCount`, rendered as a healthy line or a red
warning, populated from `filesystemReformatCount()`. §4 row #34 added; README's Maintenance dashboard
field list updated.

Verification: lint green (75 documents); native **332/332**, including `test_filesystem_mount` (all four
`decideMountOutcome` outcomes, `Reformatted` distinct from both `Mounted` and `FirstFormat`) and two
`test_dashboard` cases (healthy line, singular/plural reformat-count warning). Both device builds
(`cardputer`, `cardputer_testhooks`) compile clean, no warnings in our source. Fail-before shown by
collapsing the first-format branch to `Reformatted`: only `test_first_format_of_a_blank_partition_is_not_a_reformat`
failed, reverted.

Field validation on the reference Cardputer proved the CRK1 to CRK2 migration: a pre-0072 build wrote a
CRK1 store (sync, `first=1`), then current firmware mounted it cleanly (no `[ERROR]`, breadcrumb 0),
loaded all 28 entries (`first=0 new=0`, no `bad version` or `short record`), and re-saved as CRK2. That
run is also what surfaced the first-format case: an erased partition reports the identical `Corrupted dir
pair` mount failure as a corrupt one, so an earlier form of this seam would have logged `[ERROR]` and
breadcrumbed on every fresh device; the `fs_seen` marker fixes that.

Two adversarial passes (blind to intent, aware of law; Sonnet; read the files themselves, no recursion)
were run because this adds §4 invariant #34, new exported API, and a new persisted NVS format. The first
found no blockers on the seam; the second, on the `fs_seen` refinement, found no blockers in the decision
logic, enum/caller compatibility, or log discrimination, and one should-fix: `markFilesystemSeen()`
swallowed its NVS failures, unlike `recordReformat()` and `provisioning_store`. Fixed (it now fails loud
on an open or write failure), so a broken marker cannot silently reclassify future reformats. The
once-per-boot breadcrumb idempotency rests on the library's field-logged already-mounted short-circuit,
whose worst case is a cosmetic over-count; a dedicated verify script is deliberately not added (a corrupt
mount cannot be induced deterministically), so the glue stays review-only like all store byte code.
