---
id: '0051'
title: "CI and a pre-push hook back the pre-commit gates by scanning the pushed tree"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

The stele pre-commit hook grades the **index** — it scans `git write-tree`, not the working
tree, so a fix you forgot to stage cannot green a red commit (stele:ADR-0018). That is the right
choice for the hazard it targets, but ADR-0049's second adversarial review (Amendment 2, F4)
established that it leaves three real gaps, none closeable *inside* `pre-commit`:

- **Pathspec-limited commit.** `git commit -- <path>` (and `--only`) commits *working-tree*
  content for the named path, while the hook reads the index — so the tree that lands is not
  the tree that was checked. Reproduced against the artifact-privacy guard.
- **`--no-verify`.** Bypasses the hook entirely.
- **A clone without the hook.** The hook is installed by `/init-method`; a fork or CI checkout
  that skips it has no gate at all. The artifact-privacy guard (ADR-0049) and the doc linter
  are therefore *hook-only* — invisible to anyone who did not install the hook.

Meanwhile ADR-0004 wanted a cloud lane to build every target and run the native unit suite on
push, and §4 invariant #5 was recorded `pending (LEDGER)` because none was wired. Pushing the
repo to a public remote (slice-0050) made both gaps concrete at once: the code is now public,
anyone can open a PR from a hookless clone, and the only thing standing between a real BSSID and
the published tree was a hook that runner never installs.

The parent repo (`../adversary`) already runs exactly the missing lane — a build matrix, a
CodeQL scan, cppcheck, and Dependabot — proven green on ubuntu runners. This decision ports that
infrastructure and adds the two checks the parent has no reason to run (the stele doc linter and
the artifact-privacy guard), and adds a local `pre-push` hook as the fast mirror.

This is the `~/.claude/CLAUDE.md` §2 case again: ADR-0018's index-scan was correct for its
hazard but was never a backstop for *what reaches the remote*. Recorded as a decision that
extends it, not a silent patch.

## Decision

**A GitHub Actions CI lane and a local `pre-push` hook both scan the pushed tree, closing the
index-vs-worktree, `--no-verify`, and hookless-clone gaps the index-based pre-commit hook cannot.
CI is the authority; the pre-push hook is the fast local mirror.**

1. **CI runs on every push and pull request** (`.github/workflows/ci.yml`), on ubuntu runners
   that check out the *actual pushed/proposed ref tip* — the tree that lands, not the index a
   local hook graded. It runs four things:
   - **`guards`** — `npm run lint` (doc-corpus integrity), `npm run check:index` (the generated
     `adr/INDEX.md` matches the corpus), and `npm run check:artifact-privacy` (the ADR-0049
     guard). This is the half that closes F4: the guard now runs against the pushed tree on a
     runner nobody can `--no-verify`, not only in a hook a clone may lack. Zero-dependency node,
     so no install step.
   - **`build`** — a matrix of `pio test -e native` (host unit suite) and `pio run -e cardputer`
     (firmware build). Backs ADR-0004 lanes 1–2 and §4 #5.
   - **`cppcheck`** — a separate job gating on cppcheck's default `error` severity (null deref,
     buffer overrun, use-after-free) over our own `src/` only; ported from the parent.
   - **CodeQL** — its own workflow (`.github/workflows/codeql.yml`), a C/C++ semantic scan that
     builds the firmware to trace compilation and strips vendored `.pio/`/`lib/`/`test/` results
     before upload; ported from the parent, single-board.

2. **Single board.** Only the Cardputer ADV is a target, so the build matrix has one board leg
   (the parent's `m5stick` leg is dropped). Further boards join the matrix when slice-8 broadens
   hardware — adding a leg then is a one-line matrix entry.

3. **No hardware in CI (upholds §4 #5).** CI compiles and runs *host* tests only; every job runs
   on `ubuntu-latest`, none on a self-hosted or hardware runner. Device-in-the-loop verification
   (ADR-0004 lane 3, the on-air `verify:*` proofs) stays a manual/hardware-runner step whose
   artifact is committed for review — CI never depends on an attached device.

4. **A local `pre-push` hook** (`.claude/hooks/pre-push`) runs the *fast* half of CI before a
   push leaves the machine: the same three `guards` checks against the working tree, then the
   build half — `pio test -e native` (the host unit suite) and `pio run -e cardputer`. It is the
   fast local mirror — it catches the F4 / `--no-verify` / pathspec bypass at push time (the
   offending content is in the working tree by then) so the operator learns before the round-trip
   to CI, not after. It is **skippable** (`--no-verify`) by design: it is a convenience, not the
   authority. That is precisely why the authoritative artifact-privacy and lint checks *also* live
   in CI — a gate the operator can skip cannot be the backstop for a public remote.

5. **The native lane is made runnable at all — it was silently dead, everywhere.** The `native`
   env used a bare `platform = native`. PlatformIO resolves an unqualified platform name against
   the project working directory *before* the registry (`PlatformFactory.new`: `os.path.isdir(spec)`),
   and this repo has a top-level `native/` directory (the `-I native/mock` fake `<Preferences.h>`,
   ADR-0008). So PlatformIO read the local folder as the platform, found no `platform.json`, and
   died with `UnknownPlatform: native` on *every* host — the reference dev machine and a fresh CI
   checkout alike (the parent Adversary repo has no `native/` dir, which is why its identical CI
   native lane is green). This had been misattributed to a "corrupted local core"; it is not — the
   platform package resolves fine (`get_package('native')` succeeds), the bare-name/local-dir
   collision is the whole cause. Fixed by qualifying the spec to `platformio/native`, which skips
   the local-dir lookup; a comment at the site (stele:ADR-0012) forbids shortening it back. A
   second latent defect surfaced the instant the lane ran: `net/dashboard.cpp` was missing from
   the native `build_src_filter`, so `test_dashboard` never linked — meaning §4 invariants #21 and
   #23, both declared `verified_by: test/test_dashboard`, had a *vacuous* proof. Adding the source
   makes the suite green (267 cases) and those verifications real. With the lane genuinely runnable
   locally, native tests belong in the pre-push hook (decision 4), not only in CI.

6. **Dependabot** (`.github/dependabot.yml`) tracks the `github-actions` ecosystem only. The
   PlatformIO platform and libraries are pinned in `platformio.ini`, which Dependabot does not
   parse, and there is no pip manifest (PlatformIO is installed ad hoc in CI) — a pip or
   platformio block would have nothing to update, so it is omitted rather than shipped as dead
   config. Same reasoning as the parent.

7. **Deliberately not ported from the parent:** `release.yml` and `pages.yml`. There is no
   release process or documentation site for an alpha appliance, so both would be dead config —
   KISS (`~/.claude/CLAUDE.md` §3). They earn their place when a release/versioning decision is
   actually made.

## Consequences

- **§4 invariant #26 is added in this commit** (citing this ADR): CI and the pre-push hook run
  the doc linter and the artifact-privacy guard against the *pushed* tree on every push/PR, so
  the ADR-0049 guard is no longer hook-only and index-based. Its enforcement is the workflow
  itself; that a future edit does not *remove* those steps is review-only (the doc linter does
  not read `.github/`).
- **§4 #5 and #25 are updated** to cite this ADR: #5's cloud lane now exists and is verified
  hardware-free by the workflows (its lane-3 hardware-runner half stays pending); #25's
  `pending (LEDGER)` CI/pre-push clause is closed — the guard now runs in CI and pre-push against
  the pushed tree.
- Turning the `cppcheck` gate on surfaced (and this change fixed) two pre-existing `error`-severity
  findings — an uninitialised-frame copy in `HandshakeCollector::reset()`, and a false-positive
  linker-blob pointer subtraction silenced with a justified inline suppression, never by loosening
  the gate (slice-0052 `## As built`). A gate is only worth adding if its first run is made green.
- **Extends ADR-0004 and ADR-0018; supersedes neither.** ADR-0004's lanes are now wired;
  ADR-0018's index-scan stays correct for the pre-commit hazard — this adds the pushed-tree scan
  it was never meant to be. No mind was changed, so no supersession.
- The LEDGER's CI item (wire lanes 1–2 + lint + artifact-privacy scanning the pushed tree) is
  closed by deletion. The native-env failure is *fixed here*, not deferred, so no LEDGER item is
  added for it; the verify-script source-redaction and node-test-lane items stay open (untouched).
- **Deferred, not built here:** a hardware-runner lane for ADR-0004 lane 3 (§4 #5, still
  pending); branch protection on the remote requiring the CI checks to pass before merge (a GitHub
  setting, not a repo artifact — noted for the operator, not tracked as code).
