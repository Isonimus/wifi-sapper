---
id: '0074'
title: "LittleFS mount failure is loud and breadcrumbed, never a silent reformat"
type: architecture
status: accepted
date: 2026-10-25
supersedes: []
superseded_by: []
---

## Context

A field run on the reference Cardputer, flashing current firmware onto a device that was believed to
hold a persisted store, produced this on first boot:

```
E (5290) esp_littlefs: Corrupted dir pair at {0x0, 0x1}
E (5291) esp_littlefs: mount failed,  (-84)
E (5295) esp_littlefs: Failed to initialize LittleFS
<~5.5 s, task_wdt "task not found" repeated>
[LittleFS.cpp:59] begin(): LittleFS Already Mounted!
...
[SYNC] outcome ok=1 first=1 downloaded=28 new=0 malformed=0 overflow=0 storeError=0
```

The data partition failed to mount at its superblock (`{0x0,0x1}`), was then reformatted, and the sync
seeded 28 results into the now-empty store. The `storeError=0` is true of the blank filesystem; whatever
was there before was gone.

The destruction was caused by code we own. Every store mounted with
`LittleFS.begin(/*formatOnFail=*/true)`, which returns `false` only when the format itself fails: a
partition that fails the mount is reformatted and `begin()` returns `true`, so the caller's `[FATAL]`
never fires. The comment at the capture store's mount site read "fail loud: no place to persist" while
the behaviour was the opposite. On a device that genuinely held data, a transient corruption would wipe
its entire cracked/submitted history with no signal.

Two things the run and a follow-up established:

- The partition layout is not the cause. Measured from the flashed table: `app0` at `0x10000` is 3264K
  and holds a 1.32 MB app; the LittleFS data partition sits at `0x670000` (1536K), far from the app,
  unchanged across the versions involved. A plain `-t upload` writes only the app and otadata.
- `Corrupted dir pair at {0x0,0x1}` does not mean corruption. A later test deliberately erased the flash
  and booted: LittleFS printed the identical message on the freshly-erased partition. So that message is
  simply what LittleFS reports for any partition without a valid filesystem, a blank or erased one
  included. The original field failure was therefore most likely an **unformatted partition** (the
  device had never successfully written a store), not the panic-mid-flush corruption first suspected. The
  defect this ADR fixes stands regardless: whether a mount fails because the partition is blank or
  because it is genuinely corrupt, the old code reformatted it silently, and on a device that did hold
  data that is a silent loss.

A related gap: the sync verify reported a pass over the corrupt boot, because its error gate matches only
our own `^\[(FATAL|ERROR)\]` prefix and ignores ESP-IDF `E (...)` lines. The loud seam below closes that
for the mount case (it emits `[ERROR]`, which the existing gate catches); the broader hardening of the
verify gates against raw ESP-IDF errors is tracked in the LEDGER.

## Decision

A LittleFS mount failure fails loud, records a durable breadcrumb, then reformats and continues. The
appliance is headless-first and must keep hunting unattended, so losing a re-downloadable manifest is
preferable to halting; but the loss is never silent. Crucially, a blank partition's first-ever format is
not a loss and stays quiet.

1. **One mount seam, `ensureFilesystemMounted()`** (`net/filesystem_mount`), the single place in the
   codebase that calls `LittleFS.begin`. It attempts a non-formatting mount (`formatOnFail=false`); on
   success it returns `Mounted`. On failure it attempts a formatting mount and classifies the result
   (below). The three shipped stores and the rssi probe route through it. This is the rule-of-three hoist
   of four duplicated mount sites, and it also removes the duplicated `LittleFS Already Mounted!` warnings
   (only the first caller mounts; the rest see it mounted).

2. **The decision is a pure function**, `decideMountOutcome(firstMountOk, reformatMountOk,
   hadFilesystemBefore)`, returning `Mounted`, `FirstFormat`, `Reformatted`, or `Failed`. It is
   host-tested; the Arduino glue (`LittleFS.begin`, NVS) is device-only, proven on hardware, like all
   store byte-format and NVS code. It pins the distinction the old code lost: a reformat is its own
   outcome, not folded into a plain success.

3. **First-format is distinguished from reformat by a durable `fs_seen` marker.** A blank or erased
   partition fails the non-formatting mount identically to a corrupt one, so without a signal that a
   filesystem once existed here, every fresh or just-erased device would false-alarm "data lost" on its
   first store mount and emit an `[ERROR]` that trips the verify gates on first boot. An `fs_seen` bool in
   NVS is set on the first successful mount (clean or formatted). A mount failure is loud and
   breadcrumbed (`Reformatted`) only when `fs_seen` was already set; a failure on a never-seen partition
   is a benign first-time format (`FirstFormat`): a plain `[FS]` log, no `[ERROR]`, no breadcrumb. The
   marker write itself fails loud if NVS cannot be opened or written, so a broken marker is diagnosable
   rather than silently reclassifying every future reformat as a benign first format. Accepted edge: a
   corruption on the very first boot after upgrading from a pre-fix firmware (which never wrote `fs_seen`)
   is classified as a first format, silent, that one time; it fails toward not-false-alarming, and every
   boot thereafter is correct.

4. **A durable breadcrumb in NVS**, namespace `fshealth`, key `reformats` (a `uint32` counter),
   incremented on each `Reformatted` outcome. NVS is a separate partition, so the breadcrumb and the
   `fs_seen` marker survive both the LittleFS format they describe and a provisioning `clear()` (they are
   kept out of the `sapper` provisioning namespace deliberately). A counter, not a timestamp: the mount
   runs at boot before NTP, so any epoch would be a misleading 1970. A rising count across Maintenance
   visits is the field signal that a unit's flash is corrupting repeatedly.

5. **The Maintenance dashboard shows it.** The persisted count is read through the seam and rendered:
   healthy (`0`) or "reformatted N times (persisted data lost)". That is where an operator who physically
   holds the unit sees that a wipe happened, rather than only in a serial log nobody was watching.

6. **A standing invariant (§4 #34):** no module calls `LittleFS.begin` directly; all mounts go through
   `ensureFilesystemMounted()`. Greppable (the symbol appears in exactly one file) and checked at review.

Not chosen: failing loud and running without persistence until a human reformats (a headless appliance
that silently stops persisting is worse for this product than losing a re-downloadable manifest); and
recording the reset reason per boot (once the original failure was understood as an unformatted partition
rather than corruption, this lost most of its value; kept as a speculative LEDGER contingency).

## Consequences

- A corrupt mount on a device that had data now emits a loud `[ERROR]` the verify gates catch, so the
  silent pass of the field run cannot recur for this failure class.
- Field testing proved the on-air CRK1 to CRK2 store migration (a pre-0072 build wrote a CRK1 store;
  current firmware mounted it cleanly, loaded every entry, and re-saved as CRK2 with no `bad version` or
  `short record`). That same run is what surfaced the first-format case decision 3 handles: without it,
  the fix as first conceived would have false-alarmed on every fresh device.
- The breadcrumb reads 0 on a healthy or brand-new unit and climbs only on real reformats, so it is a
  trustworthy per-unit corruption signal, not noise.
- The seam centralises every LittleFS mount, so a later board profile or store cannot reintroduce
  `formatOnFail=true` by copy-paste; the §4 invariant and the single call site keep it in one place.
- A reformat still loses the submitted set (not re-seeded from wpa-sec, unlike the cracked set); that
  loss is now announced and counted rather than silent, but recovering it (store-and-forward of the
  submitted set off-device) remains out of scope.
