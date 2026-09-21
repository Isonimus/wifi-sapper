---
id: '0052'
title: "CI and pre-push lane — scan the pushed tree, build every target, run the native suite"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0051: wire a GitHub Actions CI lane and a local `pre-push` hook that both scan the
*pushed* tree (closing the pre-commit hook's index-only / `--no-verify` / hookless-clone gaps that
ADR-0049 F4 named), build every target, and run the native unit suite — plus fix the two latent
defects that kept the native lane from running anywhere.

- **CI (`.github/workflows/ci.yml`):** on push/PR — a `guards` job (`npm run lint`, `npm run
  check:index`, `npm run check:artifact-privacy`) against the checked-out tree; a `build` matrix
  (`pio test -e native`, `pio run -e cardputer`); a `cppcheck` job (error-severity, `src/` only).
- **CodeQL (`.github/workflows/codeql.yml` + `codeql/codeql-config.yml`):** C/C++ semantic scan,
  builds cardputer to trace compilation, strips vendored `.pio/`/`lib/`/`test/` results.
- **Dependabot (`.github/dependabot.yml`):** github-actions ecosystem only.
- **Pre-push hook (`.claude/hooks/pre-push`):** the three `guards` checks against the working
  tree, then `pio test -e native` + `pio run -e cardputer`. Fast local mirror; skippable.
- **Native-lane fixes (prerequisite):** qualify `platform = platformio/native` (the bare `native`
  was shadowed by the repo's `native/` dir → `UnknownPlatform` on every host); add
  `+<net/dashboard.cpp>` to the native `build_src_filter` (`test_dashboard` never linked).
- **Docs:** `CLAUDE.md` §4 gains row 26 and updates rows 5 and 25 to cite ADR-0051;
  `package.json` gains `check:index`; the LEDGER's CI item is closed by deletion.

## Definition of Done

**Scenario A — the native suite runs and is green (tooling).**
- **Given** the `native` env, previously dying with `UnknownPlatform: native` on every host
- **When** the spec is qualified to `platformio/native` and `net/dashboard.cpp` is added to the
  filter, and `pio test -e native` runs
- **Then** the platform resolves, `test_dashboard` links, and every test case passes.
- **Proof:** demonstrated fail-before (`UnknownPlatform`, 0 tests; then `test_dashboard` ERRORED,
  link failure) / pass-after (267 cases, exit 0). See `## As built`.

**Scenario B — the pre-push hook blocks a real BSSID that bypassed pre-commit (tooling).**
- **Given** a working tree with a real MAC-format BSSID in a file under `artifacts/` (as a
  pathspec-limited or `--no-verify` commit would leave it)
- **When** the pre-push hook runs `npm run check:artifact-privacy`
- **Then** the guard exits non-zero and the hook aborts the push before it leaves the machine.
- **Proof:** demonstrated fail-before/pass-after by hand against the guard from the working tree
  (the guard's own regression, slice-0050). See `## As built`.

**Scenario C — a clean tree passes the pre-push gates and builds (tooling).**
- **Given** the current clean working tree
- **When** the pre-push hook runs lint + index-check + artifact-privacy, then `pio test -e native`
  and `pio run -e cardputer`
- **Then** all gates pass and the push is allowed.
- **Proof:** the native suite is green (Scenario A); the cardputer build is exercised by CI and
  locally. See `## As built`.

**Scenario D — CI scans the pushed tree and uses no hardware (review + first run).**
- **Given** the workflows on a push/PR
- **When** CI runs
- **Then** the `guards` job runs the linter + artifact-privacy guard against the checked-out
  (pushed) tree — the F4-closing scan a `--no-verify` cannot skip and a hookless clone still gets —
  and every job runs on `ubuntu-latest` with no self-hosted/hardware runner (§4 #5).
- **Proof:** YAML validated locally; `runs-on` is `ubuntu-latest` in every job (grep); the first
  push to the remote exercises the workflows end-to-end (run linked in `## As built`).

## Verification

- **Native lane (Scenario A)** — `pio test -e native`, fail-before/pass-after recorded in
  `## As built`. Now genuinely exercises §4 #21/#23 (`test_dashboard`), previously vacuous.
- **Pre-push hook (Scenarios B, C)** — demonstrated fail-before/pass-after by hand. This repo has
  no wired node-test lane (all suites are Unity C++; the `read-set.test.mjs` cited by the
  hook/linter does not exist — LEDGER `audit` item), so as with slice-0050 the hook-in-anger plus
  the by-hand demonstration is the regression vehicle; the underlying checks (lint, index,
  artifact-privacy) each have their own coverage, and CI re-runs them on every push.
- **CI / CodeQL / Dependabot (Scenario D)** — YAML parse-validated; `runs-on` grep for the
  no-hardware invariant; the first remote push is the end-to-end exercise (the Actions run is the
  artifact, linked in `## As built`). A declarative workflow's correctness is that GitHub runs it;
  there is no local unit for it to regress.

## As built

Shipped as designed (ADR-0051). One discovery reshaped it: the native lane, long blamed on a "broken
local core", was dead for a different reason on *every* host.

- **Native-lane fixes (Scenario A).** `pio test -e native` died with `UnknownPlatform: native`.
  Diagnosed via the package manager directly: the platform package resolves fine
  (`get_package('native')` succeeds), but `PlatformFactory.new` checks `os.path.isdir(spec)` against
  the project cwd *before* the registry, and this repo has a top-level `native/` directory (the
  `-I native/mock` fake `<Preferences.h>`). So the bare `platform = native` was read as a local
  folder with no `platform.json` → `UnknownPlatform` — on this machine and on a fresh CI checkout
  alike (the parent Adversary repo has no `native/` dir, so its identical CI native lane is green).
  Fixed by qualifying `platform = platformio/native` (a comment at the site forbids reverting it).
  The instant the lane ran, `test_dashboard` **ERRORED** at link: `net/dashboard.cpp` was absent from
  the native `build_src_filter`, so `buildDashboardHead`/`appendCrackedRow`/`kDashboardControls`/
  `kDashboardFoot`/`maintenanceBackstopDue` were undefined — meaning §4 #21 and #23's
  `verified_by: test/test_dashboard` had been *vacuous*. Added the source. Fail-before: `UnknownPlatform`
  (0 tests), then `test_dashboard` ERRORED. Pass-after: **267 test cases, 0 failures, exit 0**.
- **Pre-push hook (Scenarios B, C).** `.claude/hooks/pre-push` runs lint + `check:index` +
  `check:artifact-privacy` against the working tree, then `pio test -e native` + `pio run -e cardputer`;
  it skips a delete-only push and fails loud if `node` or `pio` is absent. Fail-before demonstrated:
  a planted `artifacts/_prepush_selftest.txt` with a real BSSID aborted the hook at the artifact guard
  (exit 1, named file:line:MAC). Pass-after: the full hook on the clean tree passed every gate — native
  267/267, cardputer build SUCCESS — exit 0 (~70s). Regression vehicle is the hook-in-anger + this
  by-hand demonstration (no node-test lane, LEDGER audit item); the underlying checks are covered and
  CI re-runs them.
- **CI / CodeQL / Dependabot (Scenario D).** `ci.yml` (jobs: `guards` = lint + check:index +
  artifact-privacy on the checked-out tree; `build` matrix = native + cardputer; `cppcheck`),
  `codeql.yml` + `codeql/codeql-config.yml` (single-board, vendored-result filtered), `dependabot.yml`
  (github-actions only). All four YAML parse-validated; every job is `runs-on: ubuntu-latest` (grep) —
  no hardware in CI (§4 #5). `release.yml`/`pages.yml` were deliberately not ported (no release/site
  for an alpha). The first push to the remote is the end-to-end exercise — **run: _(link after push)_**.
- **Docs & wiring.** `package.json` gained `check:index`; `CLAUDE.md` §4 gained row 26 and updated
  rows 5 and 25 to cite ADR-0051; `README.md` gained the CI + CodeQL badges (now that the workflows
  exist), a `check:index` command row, and a pre-push-hook install note; the LEDGER's CI item was
  closed by deletion and a hardware-runner lane-3 item added (the node-test item's stale
  "pio broken on the current host" clause was corrected — it is fixed now).

- **Adversarial-review corrections (blind Sonnet 5 pass).** Two findings, both acted on:
  - *cppcheck had never actually been run* (unlike the native suite): its first invocation found three
    `error`-severity findings in **pre-existing** code, so the gate would have gone red on the first
    push. Two are real — `HandshakeCollector::reset()` copied an uninitialised `CapturedFrame::data[]`
    (fixed by value-init `fresh{}`, matching `retarget()`); the third, `webhook_transport_esp32.cpp`'s
    `kRootCaBundleEnd - kRootCaBundleStart`, is a false positive (the standard linker-embedded-blob
    sizing idiom) and is silenced with a justified inline `// cppcheck-suppress comparePointers` and a
    WHY comment — the gate is *not* loosened. Fail-before/pass-after: cppcheck exit 1 → **exit 0**;
    native stayed 267/267 and cardputer still builds SUCCESS after both source edits.
  - *doc inaccuracy*: rows 25/26 and the README had attributed a "pushed-tree" scan to the `pre-push`
    hook, which scans the *working* tree (a `git push origin HEAD~1:main` would pass the hook yet push
    a different tree — only CI scans the exact pushed ref). Corrected to attribute pushed-tree to CI
    and working-tree to the hook, and to name CI as the authoritative backstop.
  - The reviewer independently reproduced and confirmed correct: the `platformio/native` fix, the
    `dashboard.cpp` filter addition (hardware-free, no ODR clash), the hook's `set -e`/delete-only/
    fail-loud logic and POSIX portability, the fail-closed guards, and the ubuntu-only no-hardware
    jobs. One non-finding nit (CI `node-version` 20 → 22, past LTS) was taken.
