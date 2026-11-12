---
id: '0118'
title: "The live hunt HUD shows the lifetime cracked total from the manifest, not the run-scoped bus tally"
type: architecture
status: accepted
date: 2026-11-12
supersedes: []
superseded_by: []
---

## Context

The hunt HUD's `cracks:` counter is **run-scoped**: `ScreenToastSurface` tallies
`AppEventType::NewPassword` bus facts into a `cracks_` member since boot
(`screen_toast_surface.cpp`), so it resets to zero on every reboot. For an appliance
whose whole purpose is to be left running autonomously for days, the number an
operator standing beside the unit most wants at a glance is its **lifetime yield** —
how many networks it has ever cracked — and that number is exactly the one the HUD
throws away each power-cycle.

The lifetime total already exists on-device: the persisted `CrackedManifest` (§4 #12,
up to 256 entries) is the authoritative set of recovered networks, re-seeded from
wpa-sec on each sync, and the Maintenance dashboard already reads it through the
`CrackedManifest`/`CrackedStore` seam (§4 #21). The live HUD simply never looks at it.

Two ways to close the gap were considered:

- **Seed-then-increment:** seed `cracks_` from `manifest.size()` at `begin()`, then keep
  incrementing on `NewPassword`. Rejected: a `Changed` re-crack (ADR-0072's rotated-PSK
  re-verify) fires `NewPassword` but the manifest count stays flat (it replaces an
  entry, it does not add one), so the seeded tally drifts above the true count over the
  device's life. The displayed number would slowly lie.
- **Read the manifest count directly** each render. Always exact, because the manifest
  *is* the source of truth; no second tally to drift.

## Decision

The live hunt HUD shows the **lifetime cracked total read directly from the
`CrackedManifest`**, through a new read-only pull seam.

1. A `CrackedCountSource` abstract seam exposes one method, `crackedTotal()`, returning
   the manifest's current entry count (`CrackedManifest::size()`). `CrackedManifest`
   implements it; a fake implements it for the host tests. This mirrors the existing
   read-only pull seams the HUD already uses (`HuntSnapshotSource` for live hunt state
   ADR-0033/§4 #18, `BatteryGauge` for charge ADR-0091/§4 #40): a surface reads
   persisted/engine state only through a seam, never a parallel store parse (§4 #12) and
   never a reach into private state.

2. `ScreenToastSurface` holds an optional `CrackedCountSource*` (wired at
   `huntLoopBegin` beside `setHuntSource`), reads it once per `render()`, and folds the
   count into `ScreenView::crackedTotal`. The read is on the app task, the same task that
   applies sync results to the manifest (the drain), so unlike the cross-task hunt
   snapshot it needs no torn-read reasoning. An unwired source resolves the count to
   zero (a headless board with no screen never wires it).

3. The run-scoped `cracks_` tally and the `ScreenView::cracks` field are **removed**, not
   kept alongside: the operator chose a lifetime-only footer figure, so a second
   run-scoped number would be dead state (§3 no-dead-code). The `NewPassword` fact still
   arms the CRACKED banner (the *event*, which is inherently "this just happened"); only
   the *counter* moves to persisted truth. The two roles — a transient event banner and a
   durable total — are now sourced from the two things that actually carry them (the bus
   fact and the manifest), not both from the bus.

Only the cracked counter becomes lifetime. `up:` stays the run's terminal-upload
activity (its lifetime analogue is the `SubmittedManifest` count; surfacing that too was
considered and deferred — LEDGER — to keep this change to one seam).

## Consequences

- **One new pure seam** (`net/cracked_count_source.h`) and one line on `CrackedManifest`
  (`crackedTotal()` over the existing `size()`), both host-tested; `ScreenView` gains a
  `crackedTotal` field that must join `operator==` (ADR-0025 decision 2) or
  render-on-change silently breaks, and loses its `cracks` field.
- **The displayed cracked count no longer resets on reboot**, and no longer drifts on a
  rotated-PSK re-crack: it equals the Maintenance dashboard's count by construction (same
  seam, same source).
- Shipped inside the ADR-0116 HUD redesign slice (the footer band is being rebuilt there
  anyway), so this reuses that slice's eyes-on HUD re-proof rather than owing its own.
- A board with no cracked networks yet shows `cracked: 0`; progressive disclosure
  (ADR-0116 decision 7) is **not** applied to it (zero yield is itself information the
  operator wants, unlike a pending-queue count that is noise at zero).
