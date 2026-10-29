---
id: '0100'
title: "A node --test lane and the archive/read-scope equality guard (the missing read-set test)"
type: slice
status: accepted
date: 2026-10-29
supersedes: []
superseded_by: []
---

## Goal

Deliver the `node --test` lane decided in ADR-0099 and its first test, closing the `LEDGER.md`
audit item *"no wired node-test lane for scripts/*.mjs"*: the pre-commit hook and `lint-docs.mjs`
both cite `test/read-set.test.mjs` (asserting the hook's archive pathspec equals the linter's
exported read-set) but that file never existed and no runner ran `.mjs` tests.

This is the first release blocker (group A) before the first public release. The pre-commit hook is
a contributor's first-line guard, and an un-asserted archive/read-scope equality lets the R14/R15
drift recur silently: rules that began reading four paths the hook never archived and ran nowhere in
the hook for a full release (ADR-0018). Building the test (rather than deleting the dangling
citation) also makes the citations in the immutable ADR-0049 / slice-0050 / slice-0052 true the
moment the file exists, so no immutable-growth correction is needed (ADR-0019 untouched); a deletion
could not have reached those three immutable bodies at all.

## Definition of Done

- **Given** the pre-commit hook's archive pathspec (`.claude/hooks/pre-commit`) and the linter's
  exported `READ_SCOPE` (`scripts/lint-docs.mjs`)
- **When** `npm run test:node` runs
- **Then** `test/read-set.test.mjs` passes iff the two are equal as sets, and fails, naming the
  differing paths, if either gains or loses a path the other does not.

- **Given** the test passing on the currently-matched lists
- **When** a path is added to `READ_SCOPE` but not the hook (or the reverse)
- **Then** the test fails: proven by a temporary perturbation during development, then reverted.

- **Given** a push and a local pre-push
- **When** CI's `guards` job and the `pre-push` hook run
- **Then** `npm run test:node` runs in both and a mismatch fails the push / CI run; the index-based
  `pre-commit` hook deliberately does **not** run it (ADR-0099 decision 3), because it archives only
  `READ_SCOPE` and `test/` + `.claude/hooks/` are not in that archive.

## Design

**Existing code considered.** `READ_SCOPE` is already exported from `lint-docs.mjs` and already has a
second consumer: `.claude/hooks/post-tool-lint.mjs` reads it via `inReadScope` (ADR-0021), exported
specifically so that hook need not restate the list. The test reuses the same export for the same
reason: a restated second copy of the list is the exact defect that produced the R14/R15 drift, so
the test must read the authoritative export, never re-type the paths. The hook's side of the equality
is parsed out of `.claude/hooks/pre-commit` (the committed source that `/init-method` symlinks to
`.git/hooks/pre-commit`), not the `.git/hooks` symlink, so the test works in a fresh clone and in a
CI checkout where `.git/hooks` is empty. The pathspec is extracted from the single
`for candidate in … ; do` line by a tight regex, then whitespace-split.

**Set equality, not ordered.** The two lists happen to be in the same order today, but the invariant
is "the hook archives exactly the set the linter reads", so the test compares sorted copies and
reports the symmetric difference (`only in hook` / `only in READ_SCOPE`). Coupling the test to list
*order* would fail on a harmless reorder, a fragile test the quality bar forbids.

**`node --test`, pattern pinned.** Node's built-in runner (zero-dependency, ADR-0099 decision 1) is
wired as `npm run test:node` running `node --test test/*.test.mjs`. The glob is explicit rather than
relying on bare `node --test` recursion, so adding a nested C++ `test/test_x/` directory cannot
change what the node lane runs (ADR-0099). R11 (every `scripts/*-verify.mjs` wired in `package.json`)
does not apply: this is a `test/*.test.mjs` unit test, not a verify script.

**Not in pre-commit.** The lane runs in CI's `guards` job and the `pre-push` mirror (the full-tree
contexts), not the index-archiving `pre-commit` hook (ADR-0099 decision 3). Recorded so a later
reader does not "complete" the wiring by adding it to pre-commit, which would force archiving paths
no linter reads or grading the working tree (both against ADR-0018).

## Verification

- `test/read-set.test.mjs` under `npm run test:node` proves DoD scenarios 1 and 2. The guard is
  green on arrival (the two lists match today), so it is proven to *bite* by a temporary
  perturbation: flip one entry in `READ_SCOPE`, confirm the test fails naming that path, revert
  (recorded in `## As built`).
- Wiring (DoD 3): `npm run test:node` added to `package.json`, to the CI `guards` job
  (`.github/workflows/ci.yml`), and to the `pre-push` hook (`.claude/hooks/pre-push`). Every
  subsequent push and pre-push re-checks the equality.
- No `scripts/*-verify.mjs`: there is no device or runtime behaviour here, only a static list
  equality fully asserted by the unit test, so no verify script is added or wired.
- `npm run lint` green, `adr/INDEX.md` regenerated (ADR-0099 + this slice), the native C++ suite
  unaffected (the new lane is a separate runner).

## As built

Shipped as designed. The `node --test` lane is wired as `npm run test:node`
(`node --test test/*.test.mjs`, ADR-0099), and `test/read-set.test.mjs` asserts the pre-commit
hook's archive pathspec equals the linter's `READ_SCOPE` as sets, reporting the symmetric difference
(`onlyInHook` / `onlyInScope`) on a drift. It reuses the exported `READ_SCOPE` (the second consumer
after `post-tool-lint.mjs`'s `inReadScope`) and parses the hook's list from the committed
`.claude/hooks/pre-commit` source, not the `.git/hooks` symlink, so it runs in a fresh clone and CI.

**Wired in the two full-tree contexts, not pre-commit.** `npm run test:node` runs in the CI `guards`
job (`.github/workflows/ci.yml`) and the `pre-push` hook (`.claude/hooks/pre-push`); the index-based
pre-commit hook does not run it (ADR-0099 decision 3). Standing invariant §4 #43 records the
equality and its enforcement.

**Proven to bite (DoD scenario 2).** Injecting `'BOGUS_DRIFT_PATH'` into `READ_SCOPE` made the test
fail reporting `onlyInScope: ['BOGUS_DRIFT_PATH']`; reverting restored the pass. Fail-before /
pass-after at the guard level.

**Building, not deleting (ADR-0099).** Because the test now exists, the citations in the immutable
ADR-0049, slice-0050 and slice-0052 became true with no correction-marker append (ADR-0019
untouched). The `LEDGER.md` audit item was closed (line deleted); a `[deferred]` line tracks
back-filling the other enforcement `.mjs` onto this lane.

**Machine-checked.** `npm run test:node` 1/1, `npm run lint` 0/0 (100 documents), `adr/INDEX.md`
regenerated to carry ADR-0099 + this slice, the native C++ suite unaffected (separate runner). DoD
scenarios 1-3 are the test plus the three wiring sites.

