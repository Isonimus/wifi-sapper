---
id: '0047'
title: "The serial stimulus vocabulary — gated in the pure parser, its shipped-build absence host-tested"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

ADR-0003 split the serial control channel by transmit risk: an **observation** half (`ping`,
`state`, `dump`) that ships unflagged in every build (slice-0005), and a **stimulus / fault
injection** half behind `SAPPER_TEST_HOOKS`, "compiled out of release" so "no production Sapper
can be told over serial to deauth or upload against a chosen target" (ADR-0003 #2). That stimulus
half was deferred to "land with the engine it drives" (`LEDGER.md`); the engine now fully exists
(hunt → enqueue → drain → upload → sync → alert, through slice-0046).

Today the stimulus *primitives* already exist — `huntLoopInjectStimulus` (a wpa-sec-valid handshake
through the capture-ready seam), `huntLoopForceSyncDue` (re-arm the hourly scheduler), plus
`huntLoopInjectCrackedAlert` / `huntLoopInjectCaptureAlert` (surface facts on the bus) — all gated
by `SAPPER_TEST_HOOKS` in `hunt_loop.cpp`. But they are reachable **only from one-shot bench
probes** (`upload_probe`, `sync_probe`, `led_probe`, …), each of which *skips normal boot* and owns
the device for a single fixed scenario. The serial channel itself carries no stimulus vocabulary:
`serial_command.cpp` parses exactly `ping` / `state` / `dump`.

So the piece ADR-0003 #2 actually named — a serial vocabulary that drives *the shipped hunt loop*
(`runStationBoot` → `huntLoopBegin`, the code a released unit runs) rather than a probe surrogate —
is unbuilt, and §4 invariant #4 sits `pending (LEDGER)`: trivially satisfied only because no
stimulus vocabulary exists yet to be absent. The moment one exists, #4 needs real enforcement.

One fact about the host test lane makes that enforcement nearly free. The native unit lane compiles
`serial_command.cpp` **without** `SAPPER_TEST_HOOKS` — i.e. in its *shipped* form. `test_serial_command`
already asserts `deauth → Unknown` there. So "the stimulus vocabulary is absent from shipped builds"
is directly assertable on the always-run lane: gate the tokens behind `#ifdef SAPPER_TEST_HOOKS` and
the no-hooks host build rejects every one of them by construction.

This ADR builds the **focused** stimulus half ADR-0003 #2 names by example — a synthetic handshake
and a sync clock-advance, plus the two already-built alert injectors — and makes #4 executable. It
does **not** build the response-stub or heap/time-fault seams ADR-0003 #2 also lists; those need new
gated seams inside the shipped network/boot path and are deferred (see Consequences).

## Decision

**A `SAPPER_TEST_HOOKS`-gated stimulus vocabulary is added to the pure serial parser and dispatched
to the existing engine seams; its absence from shipped builds is host-tested as the executable teeth
for §4 #4.**

1. **Four stimulus commands, gated in the pure parser.** `serial_command.h/.cpp` gain, inside
   `#ifdef SAPPER_TEST_HOOKS`, four `CommandKind` entries and their exact-match parse arms:

   | Token | Primitive | Spine it drives |
   |---|---|---|
   | `inject-handshake` | `huntLoopInjectStimulus()` | capture → queue → wpa-sec upload |
   | `force-sync` | `huntLoopForceSyncDue()` | re-arm scheduler → cracked-results download |
   | `inject-cracked` | `huntLoopInjectCrackedAlert()` | new-password fact → LED / webhook |
   | `inject-capture` | `huntLoopInjectCaptureAlert()` | capture fact → webhook push |

   Matching stays exact and lower-case, as the observation half is (the caller is a script). The
   longest token (`inject-handshake`, 16 chars) is well within `kMaxCommandChars` (32).

   *Why these four and not the other two primitives.* `huntLoopDeferSync` and
   `huntLoopInjectHudStimulus` shape a specific *probe's* rendering/timing scenario (a dumpable
   partial-handshake HUD; a network-less bench with no STA windows), not general engine stimulus a
   harness composes against a running loop. They stay probe-only; exposing them over serial would add
   vocabulary with no running-loop meaning.

2. **The shipped-build absence is host-tested — the teeth for §4 #4.** Because the tokens and their
   parse arms live inside `#ifdef SAPPER_TEST_HOOKS`, and the native unit lane compiles the parser
   *without* the flag, `parseCommand` returns `Unknown` for every stimulus token in the shipped form.
   `test_serial_command` asserts exactly that. This is what moves #4 from `pending (LEDGER)` to
   `verified_by`: a regression that moves any token's arm outside the guard makes the no-hooks build
   recognise it, and the always-run test fails. This is the security-relevant half of the invariant —
   *absence in what ships* — and it is proven on the lane that runs on every change.

   The *positive* half (the tokens parse and dispatch **when** the flag is on) is verified on air by
   the device verify plus review — ADR-0003 #4's own split: "parsing is pure and host-tested; the
   dispatch that touches the engine, display, or network is covered by a wired `scripts/*-verify.mjs`."
   A host test cannot exercise the dispatch (it calls `huntLoopInject*`, which touch the engine and
   the radio), and the native lane is a single no-hooks build, so the hooks-on parse+dispatch is not
   re-proven on host; the absence half above is the one that guards what a released unit carries.

3. **Dispatch reaches the engine only through the existing `huntLoopInject*` seams — never a parallel
   path.** `SerialChannel::dispatch` gains gated arms that call the free functions declared in
   `hunt_loop.h`. Injecting through the *same seam a real event uses* is ADR-0003 #3 and §4 #2: an
   `inject-handshake` goes through `g_relay.onCaptureReady` exactly as a sniffed capture does, so the
   harness drives the code that ships. Each `huntLoopInject*` already guards `if (!g_running) return;`,
   so a stimulus sent before the loop reaches `Ready` is a safe no-op. Dispatch prints a `[CMD]` ack
   (ADR-0003 #6: replies are `[CMD]`, never `[ERROR]`/`[FATAL]`); the primitive's own
   `[UPLOAD]`/`[SYNC]`/`[CRACK]`/`[CAPTURE]` line is the domain evidence the verify greps.

   *Why `serial_channel.cpp` may include `hunt_loop.h`.* Driving the engine and the network flows is
   the stimulus channel's stated purpose (ADR-0003 context). `serial_channel.cpp` is already a
   device-only translation unit (it includes `Arduino.h`); the `hunt_loop.h` include and the arms that
   use it are confined to `#ifdef SAPPER_TEST_HOOKS`, so the *shipped* `serial_channel.cpp` never
   pulls in the spine and the pure `serial_command` unit that the host lane tests stays engine-free. A
   callback-seam indirection (an interface `SerialChannel` calls, wired by `main`) was rejected: for
   four commands with one caller it is the speculative over-abstraction the quality bar forbids, and it
   would not change what ships (the shipped build carries neither the arms nor the seam).

4. **Bounds and refusal are inherited unchanged (ADR-0003 #5).** The stimulus tokens flow through the
   same `CommandReader`: the 32-char bound, refuse-not-truncate, and the 64-byte-per-`pump()` drain
   already apply. This matters more now that the vocabulary can actuate — "truncating an actuation
   command into a shorter valid one would run something unasked" (ADR-0003 #5) — and the guarantee
   holds for the stimulus vocabulary with no new bounding code: a line past the bound is `TooLong`
   *before* it is parsed, regardless of what it spells.

## Consequences

- §4 invariant **#4** moves from `pending (LEDGER)` to `verified_by: test/test_serial_command`
  (shipped/no-hooks parser rejects every stimulus token) `+ scripts/0048-serial-stimulus-verify.mjs`
  (a hooks build dispatches injection over serial); `review-only` (dispatch wiring) — updated in this
  commit, its Source unchanged (ADR-0003 created the invariant; this ADR built its enforcement).
- `test_serial_command` gains the absence assertions (no new suite — the parser's tests live here).
  Fail-before/pass-after: moving any stimulus arm outside the `#ifdef` flips the shipped-build
  assertion from `Unknown` to the command.
- A new wired device verify, `scripts/0048-serial-stimulus-verify.mjs` (R11: named in `package.json`),
  writes `inject-handshake\n` to a *normally-booted* `cardputer_testhooks` unit and asserts the shipped
  hunt loop drains a capture to wpa-sec — proving stimulus drives the shipped loop, not a probe.
- **Deferred, not built here:** the response-stub seams (stub the next wpa-sec upload/download reply)
  and the heap/time-fault injectors (`heap-too-low` / `time-not-synced`) of ADR-0003 #2. Each needs a
  new `SAPPER_TEST_HOOKS` seam *inside* the shipped uploader / fetcher / boot path, with its own
  shipped-absence guarantee — a larger surface than this focused slice, and the probes already
  exercise the real wpa-sec classification on air. Recorded as a narrowed `LEDGER.md` item citing
  ADR-0003 and this ADR.
- Does not supersede ADR-0003: it implements ADR-0003 #2's deferred stimulus half (the two categories
  ADR-0003 names by example) and realises the enforcement ADR-0003 #4 always intended.
