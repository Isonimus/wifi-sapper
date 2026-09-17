---
id: '0004'
title: "Firmware verification harness — native tests and compile in CI, device verify on hardware"
type: architecture
status: accepted
date: 2026-09-17
supersedes: []
superseded_by: []
---

## Context

`docs/quality-bar.md` §3 requires that any behaviour a unit test cannot assert ships a
`scripts/<slice>-verify.mjs` that drives the *real* system headlessly, fails on any error,
writes artifacts for human review, is wired into `package.json`, and **has its error-check
half run in CI**. That rule was written for a browser application, where "the real system"
is a page a headless browser loads on the same machine the CI job runs on. Two facts about
this project break the "runs in CI" clause as written:

- **The real system is a microcontroller, not a process.** Driving it headlessly means
  flashing a firmware image to an ESP-32 and talking to it over a serial port — exactly the
  channel ADR-0003 exists to provide. A cloud CI runner has no ESP-32 attached, so a
  device-in-the-loop verify script cannot run there at all.
- **The two halves of §3 have different homes on firmware.** §3's error-check half
  (pass/fail, machine-checkable) and its artifact half (screenshots/dumps, human-reviewed)
  can run in one place for a browser app. On firmware they split cleanly by what needs
  hardware: pure logic and a link of the image need none; anything about runtime behaviour
  on the panel or the radio needs the board.

The Adversary already demonstrates the hardware-free half is real: its `native` env
(`platform = native`, Unity) compiles and unit-tests `core/`, `utils/`, and the network and
storage modules with `hal/` and `ui/` excluded — the same headless-first boundary ADR-0001
draws for the Sapper. What is *not* yet decided is what "runs in CI" means for the half that
genuinely needs a Cardputer on the end of a USB cable.

## Decision

The §3 harness is realised in three lanes, split by what each needs to run:

**1. Native unit tests — run in CI, every push.** The `native` env (Unity, `platform =
native`) compiles and tests everything ADR-0001 made hardware-independent: the HuntEngine,
the pure serial-command parser (ADR-0003 #4), Board Profile capability queries, and the
network/upload logic. This is the primary regression gate and it runs unattended in cloud
CI with no hardware.

**2. Board compile — run in CI, every push.** Each `SAPPER_BOARD_*` env is built to a linked
`.elf` in CI (compile + link, not flash). This proves the firmware still builds against the
pinned pioarduino platform, Bruce's patched libs, and the `weaken_deauth_pre.py` link step
(ADR-0001) — the toolchain breakage that a native-only gate would miss entirely. It asserts
nothing about runtime behaviour; it asserts the image exists and links.

**3. Device verify — run on attached hardware, not cloud CI.** A `scripts/<slice>-verify.mjs`
flashes the board, drives it over the ADR-0003 serial channel, and asserts runtime
behaviour: it **fails on any `[ERROR]`/`[FATAL]` line** (§3's machine-checkable half, here
watching the serial stream instead of a browser console) and **writes artifacts** — panel
dumps, measured heap, captured `[STATE]` snapshots — committed under a known path for human
review (§3's eyes-on half). It runs on a workstation with the board attached, or a
self-hosted runner that has one; it is **never** a cloud-CI gate, because a check that
depends on hardware the runner lacks is worse than no check.

**4. `package.json` wires all three, and R11 still bites.** The verify scripts are wired
into `package.json` npm scripts exactly as §3/R11 require, so an unwired `*-verify.mjs` still
fails the linter. The device lane is a distinct npm script (e.g. `verify:device`) from the
CI lane (`test:native`, `build:boards`), so "wired and runnable on hardware" and "gates
cloud CI" stay separately visible. `package.json` does not exist yet; it is created with
slice-1, the first slice to ship a verify script.

*Why `package.json` and not a `Makefile`, on a firmware project.* The registry is not a web
toolchain — there is no bundler or framework, and the build/test lanes are plain `pio`
invocations the npm scripts shell out to (`pio test -e native`, `pio run -e cardputer`). Two
facts pin it to `package.json` anyway: the verify scripts are Node `.mjs` by method
convention and need a manifest to declare their one dependency (a serial library) and to be
invoked; and R11 — the linter's only harness-wiring guarantee, the check that caught 11 of 12
dead scripts — reads `package.json` and nothing else. A `Makefile` in its place makes
`existsSync(package.json)` false, so R11 returns early and an unwired verify script passes
lint forever: the automated guarantee is traded for a hand-checked file. A `Makefile` whose
targets *call* the npm scripts is a fine ergonomic alias if wanted, because it keeps one
registry; a `Makefile` that *replaces* `package.json` is the one shape rejected here.

**5. The lanes map onto ADR-0003's risk split.** The device verify script talks to the
**observation** half of the serial channel, which ships unflagged — so what it exercises is
the real shipped binary's behaviour. A verify step that needs injected stimulus builds the
board env with `SAPPER_TEST_HOOKS` for that run; the resulting artifact is explicitly a
test-binary artifact, per ADR-0003's stated limit.

## Consequences

- CI stays hardware-free and fast: a broken engine, a broken parser, or a broken toolchain
  link fails in the cloud on every push; only genuine runtime behaviour waits for a board.
- The cost is honest and stated: runtime regressions (a panel that inits wrong, a capture
  that never fires) are caught by the device lane, which a human must run on hardware and
  whose artifact a human must read. This is the irreducible part of verifying a physical
  appliance; §3's promise that *the error-check half runs unattended* is kept for lanes 1–2
  and explicitly waived for lane 3, not silently dropped.
- `package.json` and the first `verify:device` / `test:native` / `build:boards` scripts land
  in slice-1. Until then there is no verify script, so R11 has nothing to check (it returns
  early with no `package.json`), which is correct: the debt is the slice's, not a hidden gap.
- This ADR creates one standing invariant, added to `CLAUDE.md` §4 in the same commit that
  accepts it: no cloud-CI lane may depend on attached hardware — device-in-the-loop
  verification runs on a hardware lane and its artifact is committed for review.
