---
id: '0103'
title: "Bench-gated drain-outcome summary line on the hunt path"
type: architecture
status: accepted
date: 2026-10-29
supersedes: []
superseded_by: []
---

## Context

ADR-0021 made the `SerialEventLogger` consume `DrainStarted`/`DrainCompleted` silently (the
`break` at `hunt_loop.cpp`), keeping its serial output to the `[SYNC]`/`[CRACK]`/`[CAPTURE]`
domain lines the sync/capture verifies grep for. The drain counters still reach surfaces (the
LED) over the bus and the harness over the `lastDrain()` pull snapshot; the **only** serial
rendering of a `DrainOutcome` is `upload_probe.cpp`'s `[UPLOAD] drain done …` line, emitted only
under `SAPPER_TEST_UPLOAD` (the bench upload probe). Two concrete failures follow from that one
omission:

- **The gate-C observability deadlock.** Gate C (ADR-0065 #30) writes `markSubmitted` only on a
  terminal `Accepted`/`Duplicate`, and the `skippedSubmitted` suppression it then applies is
  counted only inside a drain outcome. The upload probe is **disarmed** and injects synthetic
  handshakes that wpa-sec always classifies `Rejected`, so it can never produce the
  `Accepted` → `markSubmitted` → `skippedSubmitted` sequence that proves the gate. The **shipped
  hunt loop** can land a real terminal upload and then skip a re-queued already-submitted BSSID,
  but it renders no drain summary to serial, so the `skippedSubmitted` datum that would prove gate
  C never reaches an observer. Gate C is therefore unprovable on air with the logging as it stands.

- **The `verify:serial-stimulus` bug (`scripts/0048`, LEDGER).** That script's own precondition is
  a **normally-booting** hooks build (real credentials, no probe env), and it waits for an
  `[UPLOAD] drain done` line. On that build the probe is inactive and the shipped-loop logger
  `break`s on the drain events, so the awaited line never appears and the script times out. It was
  never caught because 0048 is a hardware-lane script that has not been run on air.

ADR-0101 already settled the *mechanism* for a single verbose line that must be present on a bench
build yet absent from the shipped/release image: **one targeted compile flag at that one site**,
not a log-level tiering system. It explicitly deferred the drain-summary line's own merits to this
ADR ("that remains an ADR-0021-reversing decision to be taken on its own merits").

## Decision

Emit the `[UPLOAD] drain done …` summary from the shipped-loop `SerialEventLogger`'s
`DrainCompleted` handler, behind a single `#ifdef SAPPER_TEST_HOOKS` at that one site (ADR-0101's
mechanism). The line's format is **identical** to the upload probe's existing drain line (the same
`[UPLOAD] drain done` prefix and the same ten `DrainOutcome` fields), so a drain renders the same
recognisable record whether it comes from the probe or from the real hunt loop.

The effect splits cleanly by build:

- A **`*_testhooks` build** (the bench/verify builds, and the normally-booting shipped-shaped build
  that `verify:serial-stimulus` and the gate-C proofs use) prints the drain summary from the real
  hunt loop. `skippedSubmitted` / `skippedCracked` / `accepted` / `duplicate` / `rejected` are now
  observable on the shipped hunt path, so gate C is directly provable and 0048 sees its awaited
  line.
- A **shipped/release build** (no `SAPPER_TEST_HOOKS`) compiles the line out. The release image
  stays byte-clean and the shipped serial output stays the minimal domain set ADR-0021 intended:
  the binary an operator flashes is the verified binary in everything but the hooks vocabulary
  (ADR-0101).

`scripts/0048`'s drain is also made **deterministic**. `inject-handshake` enqueues through the real
capture-ready seam but does not itself open an STA window (a window opens on `drainThreshold`=8, the
5-minute `maxDrainIntervalMs`, or a due sync). So 0048 sends `force-sync` right after
`inject-handshake` to open a window promptly, and accepts only a drain that completes **after** the
stimulus and carries a wpa-sec result, so a pre-stimulus boot-sync drain (empty queue, no upload)
cannot satisfy the assertion.

### Why this does not supersede ADR-0021

ADR-0021's load-bearing decisions all stand unchanged: the `EventBus` boundary, the capture→queue
data path staying off the bus (§4 #13), the push-seam migration, and the pull snapshot for the
harness. This ADR refines only the `SerialEventLogger`'s incidental output policy (the "stays
sync-only" code choice), adding one compile-gated line. Nothing ADR-0021 decided is reversed, so
there is no superseding record to write and its account stays true for its date. The LEDGER framed
this as "reverses ADR-0021's drain-facts-off-serial choice"; the accurate statement is narrower,
the line appears in the shipped hunt **code path** under the hooks flag, never in the shipped
**image**.

### Why not extract a shared formatter

The drain line now has two call sites (`upload_probe.cpp` and the `SerialEventLogger`). Rule of
three (quality bar §3) sanctions duplication at the second instance, and the two lines are consumed
by **different** verify scripts, `verify:upload` reads the probe's, `verify:serial-stimulus` and the
gate-C proofs read the logger's, so they carry no cross-call correctness dependency a shared
function would protect. Every other `SerialEventLogger` line (`[SYNC]`/`[CRACK]`/`[CAPTURE]`) is an
inline device `printf` verified on air, not host-tested; the drain line follows that established
pattern. A cross-reference comment at both sites (citing this ADR) records that the two formats are
kept identical on purpose, the review-only guard the repo already uses for such couplings. A shared
pure formatter is the move on a *third* site, not this one (LEDGER if it arrives).

## Consequences

- **The gate-C observability deadlock is broken.** Any board's `*_testhooks` build can prove gate C
  directly: land a real terminal upload, re-queue the same BSSID, and read `skippedSubmitted` off
  the shipped hunt path's drain line, no reliance on the disarmed, `Rejected`-only upload probe.
  `docs/on-air-proof-runbook.md` gains the hooks-hunt-build gate-C path alongside the upload-probe
  path.
- **`scripts/0048` passes** on a normally-booting hooks build, closing the LEDGER `[bug]`.
- **Other hooks-build verifies** (`verify:sync`, `verify:led`, …) now see an extra
  `[UPLOAD] drain done` line per drain cycle. It is informational (never `[ERROR]`/`[FATAL]`) and
  none of them assert the absence of `[UPLOAD]` lines, so they are unaffected.
- **No native/host test change.** The line is device serial output in the lane-3 model, verified by
  0048, exactly as the other `SerialEventLogger` lines are verified on air.
- **Enforcement is review-only**, and this adds **no §4 row.** The `#ifdef SAPPER_TEST_HOOKS` at the
  one site is the structural guarantee the line is absent from a shipped image, visible at review.
  It is deliberately **not** folded into §4 #4: that invariant is the *security* guarantee that the
  serial actuation/stimulus **vocabulary** (`inject-handshake`/`force-sync`/…) cannot exist in a
  shipped binary, asserted executably by compiling the parser without the flag. The drain line is
  observability, carries no capability, and is not a parser token, so #4's executable lane does not
  reach it and widening #4 to a non-security diagnostic would blur what it guarantees. The on-air
  proof that the line now appears on the hooks hunt path is `scripts/0048`, wired in `package.json`;
  its shipped-absence is the compile gate, reviewed.
