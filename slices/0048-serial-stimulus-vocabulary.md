---
id: '0048'
title: "Serial stimulus vocabulary — drive the shipped hunt loop over serial"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0047: add a `SAPPER_TEST_HOOKS`-gated stimulus vocabulary to the pure serial parser so
the on-air harness can drive the *shipped* hunt loop (`runStationBoot` → `huntLoopBegin`) over serial,
and make §4 invariant #4 executable — the always-run no-hooks host lane proves the shipped parser
rejects every stimulus token.

- **Pure parser (`serial_command.h/.cpp`):** four gated `CommandKind` entries + exact-match parse
  arms, all inside `#ifdef SAPPER_TEST_HOOKS`: `inject-handshake`, `force-sync`, `inject-cracked`,
  `inject-capture`.
- **Dispatch (`serial_channel.cpp`, device-only, gated):** arms that call the matching free functions
  in `hunt_loop.h` (`huntLoopInjectStimulus` / `huntLoopForceSyncDue` / `huntLoopInjectCrackedAlert` /
  `huntLoopInjectCaptureAlert`) — the same seams the probes call (§4 #2) — and print a `[CMD]` ack.
- **Enforcement:** `test_serial_command` asserts every stimulus token → `Unknown` in the shipped
  (no-hooks) parser; a new wired `scripts/0048-serial-stimulus-verify.mjs` drives a normally-booted
  hooks build over serial. §4 #4 moves from `pending (LEDGER)` to `verified_by`.

## Definition of Done

**Scenario A — the shipped parser rejects every stimulus token (host).**
- **Given** `serial_command.cpp` compiled **without** `SAPPER_TEST_HOOKS` (the native unit lane — the
  shipped form)
- **When** `parseCommand` is given each of `inject-handshake`, `force-sync`, `inject-cracked`,
  `inject-capture`
- **Then** each returns `CommandKind::Unknown` — the actuation vocabulary is absent from what ships,
  by construction (the tokens live inside the `#ifdef`). A regression that moves any arm outside the
  guard makes the no-hooks build recognise it and this test fails.
- **Proof:** `test_serial_command` gains `test_stimulus_vocabulary_gated_by_hooks` — under the
  no-hooks/shipped compilation it asserts every token → `Unknown` (fail-before/pass-after).

**Scenario B — the vocabulary parses and dispatches under hooks (review + device).**
- **Given** a `cardputer_testhooks` build
- **When** each stimulus token is fed to the parser and dispatched
- **Then** it parses to its gated `CommandKind` and `SerialChannel::dispatch` calls the matching
  `huntLoopInject*` seam, printing a `[CMD]` ack.
- **Proof:** review of the gated parse arms + dispatch wiring; `scripts/0048-serial-stimulus-verify.mjs`
  on air (device-only, review-only per ADR-0003 #4).

**Scenario C — a stimulus command drives the shipped hunt loop over serial (device).**
- **Given** a `cardputer_testhooks` unit **normally booted** into the hunt loop (no probe env set),
  associated to WiFi
- **When** the verify writes `inject-handshake\n` to the port
- **Then** the shipped loop drains a synthetic capture to wpa-sec (`[UPLOAD] drain done`, associated,
  hunt resumed) — proving the stimulus drives the *shipped* loop, not a probe surrogate.
- **Proof:** `scripts/0048-serial-stimulus-verify.mjs` (wired `verify:serial-stimulus`), artifact
  committed for review.

**Scenario D — refuse-not-truncate protects the actuation vocabulary (host).**
- **Given** the `CommandReader`
- **When** a line longer than `kMaxCommandChars` that begins with a stimulus token is fed
- **Then** the completed line is reported `TooLong`, never the stimulus command a prefix spells —
  ADR-0003 #5's guarantee, now guarding an actuation command.
- **Proof:** `test_serial_command` gains `test_overlong_stimulus_prefix_refused`.

## Verification

- **Host (`test_serial_command`, native lane)** — Scenario A (every stimulus token → `Unknown` in the
  no-hooks/shipped parser: the §4 #4 teeth) and Scenario D (an over-length stimulus prefix →
  `TooLong`). The pure parser's tests already live here, so its stimulus assertions belong here too —
  no new suite (§3, cohesion).
- **Device (`scripts/0048-serial-stimulus-verify.mjs`, wired `verify:serial-stimulus`, R11)** —
  Scenario C: a normally-booted hooks unit receives `inject-handshake` over serial and drains to
  wpa-sec. The error-check half (a `[FATAL]`/`[ERROR]` line or a port error fails the run) is
  machine-checkable and runs unattended; the drain artifact is committed for human review. The on-air
  run is a hardware step (no cloud lane depends on attached hardware, §4 #5).
- **Review** — Scenario B: the gated parse arms, the `#ifdef`-confined `hunt_loop.h` include and
  dispatch arms in `serial_channel.cpp`, and the `[CMD]` acks are device-only wiring behind
  `SAPPER_TEST_HOOKS` (review-only, like every gated dispatch path — ADR-0003 #4).

## As built

Shipped as designed (ADR-0047). No design changes during implementation.

- **Pure parser (`serial_command.h/.cpp`):** four `CommandKind` enumerators (`InjectHandshake`,
  `ForceSync`, `InjectCracked`, `InjectCapture`) appended *after* the observation kinds — so the
  shipped enumerator values are unchanged — and four exact-match parse arms, all inside
  `#ifdef SAPPER_TEST_HOOKS`. The file-level doc comment now states the gating and the host-test teeth.
- **Enforcement (`test_serial_command`):** `test_stimulus_vocabulary_gated_by_hooks` asserts each
  stimulus token → `Unknown` in the no-hooks (shipped) compilation the native lane performs, and →
  its gated kind under `SAPPER_TEST_HOOKS` — correct whichever way the TU compiles.
  `test_overlong_stimulus_prefix_refused` proves an over-length line beginning with a stimulus token
  is `TooLong`, never a truncated actuation command (ADR-0003 #5). Fail-before/pass-after was
  demonstrated: leaking the `inject-handshake` enumerator + parse arm out of the `#ifdef` made the
  no-hooks build recognise it and the gated-vocabulary test failed (`Expected 4 Was 6`), then reverted.
- **Dispatch (`serial_channel.h/.cpp`, device-only + gated):** `dispatch()` gained four arms behind
  `#ifdef SAPPER_TEST_HOOKS` that ack `[CMD] <token>` (ADR-0003 #6) and call the matching
  `huntLoopInject*` free function; the `#include "hunt_loop.h"` is confined to the same `#ifdef`. Each
  injector already no-ops before `g_running`, so a premature stimulus is harmless. The `-Wswitch`
  exhaustiveness holds in both builds (the cases exist exactly when the enumerators do).
- **Device verify:** `scripts/0048-serial-stimulus-verify.mjs` (wired `verify:serial-stimulus`, R11)
  writes `inject-handshake` to a *normally-booted* hooks unit and asserts the shipped loop drains a
  capture to wpa-sec; its `[FATAL]`/`[ERROR]`/port-error half is machine-checkable, the drain artifact
  is committed for review. On-air run is a hardware step.
- **Docs:** `CLAUDE.md` §4 #4 moved from `pending (LEDGER)` to `verified_by` (Source still ADR-0003,
  enforcement built by ADR-0047); the broad `LEDGER.md` stimulus item narrowed to the unbuilt
  response-stub/heap-time-fault remainder; `README.md` gained the `verify:serial-stimulus` row.
- **Host:** all 27 native suites pass, warning-clean under `-Wall -Wextra` (`test_serial_command` now
  11 tests). **Boards:** both build clean under `-Wall -Wextra` — shipped `cardputer` unchanged
  (Flash 1372551 B), `cardputer_testhooks` +2268 B for the gated dispatch (Flash 41.1%, RAM 35.6%).
- **Adversarial pass (Sonnet 5, blind to intent, aware of law):** no findings. It confirmed by reading
  `platformio.ini` that `env:native` compiles only `serial_command.cpp` (never `serial_channel.cpp`),
  so the `hunt_loop.h` include never reaches the host lane; that the enum-append preserves `-Wswitch`
  exhaustiveness in both builds; that the enforcement test is non-vacuous (asserts the exact no-flag
  compilation the lane performs); and that all four injectors guard on `g_running` with dispatch
  running single-threaded from `loop()` (no ISR/concurrency hazard). It noted the test's hooks-on
  branch is review-only under current CI — matching ADR-0047 decision 2.
