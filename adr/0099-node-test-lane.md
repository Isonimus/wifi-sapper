---
id: '0099'
title: "A node --test lane for the repo's .mjs tooling, and the archive/read-scope equality it first asserts"
type: architecture
status: accepted
date: 2026-10-29
supersedes: []
superseded_by: []
---

## Context

The repo's enforcement machinery is a growing body of zero-dependency `.mjs`: the doc linter
(`lint-docs.mjs`), the index builder, the immutable-growth check, the artifact-privacy guard, and
~20 `scripts/*-verify.mjs` / probe scripts. **None has a unit test.** Every test suite in the repo
is Unity C++ run under `pio test -e native`; nothing executes a `.mjs` test.

ADR-0018's pre-commit hook archives only the paths the checks *read*, not the whole tree: a measured
cost decision (the full tree is ~1.1 ms/file, ~11 s on a 10k-file repo, which is a hook people
delete). That archive pathspec lives in `.claude/hooks/pre-commit` (the `for candidate in …` list)
and must stay equal to the set the linter actually reads, exported as `READ_SCOPE` from
`lint-docs.mjs`. If the linter grows to read a path the hook does not archive, the hook materialises
a staged tree missing that file and the rule runs against nothing.

That exact drift shipped. Rules R14/R15 began reading four prose paths the hook never archived and
were **dead in the hook for a full release** while green in a working-tree run (ADR-0018 records it).
The guard written in response was a comment in *both* files citing `test/read-set.test.mjs` as the
thing that "holds the two lists equal". But that file was never written and no runner ran `.mjs`
tests, so the equivalence rested on a comment, and the citation dangled (tracked in `LEDGER.md`,
and in turn cited by ADR-0049, slice-0050, slice-0052). Preparing the repo for a public remote made
the gap a release blocker: the pre-commit hook is a contributor's first-line guard, and a silently
un-asserted archive/read-scope equality lets the R14/R15 class recur unseen.

## Decision

1. **`.mjs` unit tests live in `test/*.test.mjs` and run under Node's built-in test runner**
   (`node --test`), wired as `npm run test:node`. Node's runner is chosen for the reason the linter
   takes no YAML dependency (ADR-0003): it is zero-install, shipped with the Node the hooks already
   require. No Jest/Vitest/Mocha pulled in for a handful of pure-logic assertions.

2. **The first test is `test/read-set.test.mjs`.** It imports `READ_SCOPE` from `lint-docs.mjs`,
   parses the archive pathspec out of `.claude/hooks/pre-commit` (the committed source of the hook,
   not the local `.git/hooks/pre-commit` symlink, so it works in a fresh clone and in CI where
   `.git/hooks` is empty), and asserts the two are **equal as sets**. A future edit that adds a path
   to one list but not the other fails this test, naming the offending paths. It reuses the exported
   `READ_SCOPE` rather than restating the list, because a restated second copy is the very defect
   `READ_SCOPE`'s own doc warns against (it is why `inReadScope` is exported for
   `.claude/hooks/post-tool-lint.mjs`, ADR-0021).

3. **The lane runs in the two full-tree contexts, not in pre-commit.** It is wired into CI's
   `guards` job (the authority: pushed tree, unskippable, ADR-0051 / §4 #26) and the `pre-push`
   mirror. It is deliberately **not** wired into the `pre-commit` hook. That hook grades the *commit*
   by archiving only `READ_SCOPE` into a temp tree (ADR-0018); `test/` and `.claude/hooks/` are not
   in that archive. Wiring the test there would force one of two regressions: bloating the archive
   with paths no linter reads (losing ADR-0018's cost decision), or reading the working tree instead
   of the commit (the exact "pass on a state no commit contains" failure ADR-0018 exists to
   prevent). CI and pre-push both run against a full checkout, where the test reads the real
   committed files with no archive involved.

## Consequences

- The archive/read-scope equality becomes machine-asserted rather than comment-asserted: new
  standing invariant §4 #43, added in this commit.
- Building the test (rather than deleting the dangling citation) makes the existing citations in the
  immutable ADR-0049 / slice-0050 / slice-0052 **true** the moment the file exists, so no
  correction-marker append is needed and the immutable-growth rule (ADR-0019) is never touched. A
  deletion could not have reached those three immutable bodies at all.
- A home now exists for the script-level unit tests the enforcement `.mjs` have always lacked: the
  artifact-privacy guard's regex classes, the index builder's frontmatter parse, and pure helpers
  that today live only in `.mjs` become testable here. Back-filling them is deferred (`LEDGER.md`).
- `node --test` discovery defaults are version-sensitive, so the lane pins the pattern explicitly
  (`test/*.test.mjs`) rather than relying on bare `node --test` recursion; adding a nested C++
  `test/test_x/` directory therefore cannot change what the node lane runs.
- The LEDGER audit item (no wired node-test lane; dangling `read-set.test.mjs`) is closed by
  slice-0100, which deletes the line.
