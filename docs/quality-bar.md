# Quality bar

The standard a slice's `## Definition of Done` is measured against (stele:ADR-0024). This
file ships with the method and is vendored into this repo; it is a **live doc**
(stele:ADR-0010) — update it in the same change as the practice it describes.

It is yours to adapt. An `--update` keeps an edit you made here and reports it, and
overwrites this file only if the repo never touched it (stele:ADR-0023). A rule that does
not apply to this stack should be cut rather than ignored.

Each section says how it is **enforced**, in the vocabulary of `CLAUDE.md` §4 —
`verified_by: <script>`, `pending (LEDGER)`, `review-only`. Most of this is `review-only`:
it is enforced by `/wrap-up` and by review, not by a script. That is stated rather than
glossed, because a standard that overstates its own enforcement is the failure
stele:ADR-0003 exists to prevent, committed one level up.

## 1. Correctness — `review-only`

- **Never use `any`.** No exceptions. `unknown` plus narrowing, generics, or proper types.
  In a language without `any`, the rule is the same one: no escape hatch that turns off the
  checker for a value you did not want to describe.
- **Fail loud.** No swallowed errors — no empty `catch`, no catch-log-and-continue past a
  broken invariant, no default or fallback that masks a real failure. Validate at boundaries
  and stop at the first sign something is wrong. A silenced error is a bug debugged twice:
  once now, blind, and once later for real.
- **No hacks, no quick fixes, no workarounds.** Find the root cause and fix it for good. A
  symptom silenced is a bug rescheduled.
- **No magic values.** A bare `86400` or `0.15` in a branch is a latent bug — name it as a
  constant whose name explains what it is and why.

## 2. Design — `review-only`

- **Lean, purposeful code — KISS.** The simplest thing that works. Complexity must earn its
  place, and unexplained complexity is a defect. No speculative generality, no abstraction
  without a present need. Fight cyclomatic complexity by extracting composable, well-named
  functions **for readability and reuse — never to hit a number**; decomposition that adds
  indirection without adding clarity is its own smell.
- **Cohesion and DRY, by the rule of three.** Modules stay cohesive; a piece of logic lives
  in one place. Reach for reuse or composition on the **third** instance of the same logic —
  not the first, which is the speculative generality forbidden above. Apply SOLID only where
  it earns its keep; misapplied, it produces exactly the over-abstraction this bar exists to
  prevent.
- **Names are accurate descriptors.** No `x`, `tmp`, `data2`, `handle2`. A name states the
  thing's intent and its meaning in the domain. When a good name is hard to find, the design
  is usually the problem, not the vocabulary.
- **Comments explain WHY, not WHAT** — the code already shows what. No commented-out code and
  no unused exports left behind; dead code is deleted, not parked. Version control remembers
  it.
- **Never hardcode absolute paths** in config files, scripts, or commands. Always relative.
- **Prefer the established solution; argue any departure in writing** (stele:ADR-0025). Where a
  well-tested library, pattern, protocol, or industry standard already covers the need, propose
  it first and **by name**, before drawing a bespoke design — including when the operator asked
  for the bespoke build, since asking for one is not evidence that none exists. Hand-rolling is
  the exception and it is a legitimate one, under two conditions together: it names the property
  the standard would cost (a hard constraint, a dependency budget, a platform or licence limit)
  **and** it states the build cost as a measurement, not an estimate. What is not admissible is
  reinventing by default, or meeting the standard after the code is written — at that point the
  sunk build argues for itself and the comparison is a design against a rewrite. Both cases are
  on the record in the repo that ships this file: `stele:ADR-0022` declines a mutation-testing
  framework, naming the zero-dependency property it would cost and the twenty lines its
  replacement takes; `stele:ADR-0008` is the bill for the omission — a design that assumed the
  pre-commit slot was free met the widely-deployed tool that owns it three ADRs later, and pays
  for a second install shape permanently. Inside a single codebase the same rule is
  `stele:ADR-0013`.

## 3. Design first — measure twice, cut once — `verified_by: lint-docs.mjs` (partly)

Design before implementation. The decision is written down *before* the code, and the doc
and the code ship in the same commit. R12 and R13 check that a slice carries its
`## Verification` and its `## Definition of Done`; they cannot check that a scenario is
*right*, which is what `/wrap-up` is for.

**Decisions cite measured data, not estimates.** If a design question has a measurable
answer, measure it — that is what `scripts/<topic>-probe.mjs` is for, and the number goes in
the ADR.

For anything non-trivial, **present the options and their tradeoffs before building.**

## 4. Tests — `review-only`

- **Every relevant piece of logic gets a regression test — a good one.** No excuses, and no
  irrelevant, duplicate, or fragile tests either. Test observable behaviour, not
  implementation internals, and cover the error and edge paths, not just the happy one. Tests
  are codebase: same standards, same strict typing.
- **A regression test must fail before the fix and pass after.** One that passes before
  proves nothing.
- **Tests assert intended behaviour, not observed behaviour** (stele:ADR-0024). A test
  derives from the spec — the slice's `## Definition of Done`, the API contract, the
  reference implementation, the issue — never from reading the code and recording what it
  currently returns. A test written from the code cannot fail when the code is wrong: it
  detects change, and it will pin a bug in place and defend it against the fix.
  - Where there is no prior behaviour to fail against, **write the expected value down before
    running it**, and treat a first-run pass as unverified rather than as proof.
  - Where no specification exists, **say so at the test site** and name what the expectation
    is derived from instead. An invented citation is worse than an admitted gap.
  - When the spec is ambiguous, **settle the spec.** Never let the implementation cast the
    deciding vote.
  - **Citing the source is not the same as opening it.** The finding behind this rule was a
    test whose comment cited the reference implementation by class name and then stated the
    wrong arithmetic. Read the source, not the writeup — including your own. An aggregated
    report of somebody else's findings is a writeup too.
  - **Never assert a value the test imported from the module under test.** `expect(f(x)).toBe(K)`
    where `K` comes from the module that produced it is the assertion `K === K`; it passes
    whatever `K` is, including wrong. Write the number, and put the derivation in a comment.
    This is the one part of this rule a machine can find, and in the suite that motivated it
    the form appeared six times — more often than any other test-basis defect
    (stele:ADR-0024). If this repo has an AST linter, it should carry the rule; scope it to
    **scalar constants**, since asserting an imported *enum member* (`toBe(ItemId.Apple)`) is
    correct and was 594 of the hits when the scope was widened.

Neither instrument in the coverage layer detects a test derived from the implementation.
`npm run mutants` cannot: such a test kills its mutant perfectly well, because mutating the
constant breaks the test that asserts the constant (stele:ADR-0022). The adversarial pass in
`/wrap-up` is the enforcement (stele:ADR-0017) — except for the imported-constant form above,
which is the rule's one mechanically checkable subset, and catching some instances of a defect
beats catching none.

## 5. Finishing — `review-only`

- **Boyscout rule:** leave every touched file better than found. A noticed bug is ours even
  if we did not introduce it — fix it, or log it in `LEDGER.md` if deferred.
- **Verify before declaring done.** Never report a task complete without running this repo's
  own checks — typecheck, linter, tests. "Done" means the checks ran and passed in this
  conversation, and that what ran was said out loud — not that the change looks right.

## 6. Commits — `review-only`

- **Explain WHY, not WHAT.** The diff already shows what changed.
- **Atomic and coherent** — one logical change per commit.
- **Ship the doc and the code in the same commit.**
- **Commit only when asked.** Never commit unprompted.

## 7. The operator may be wrong — `review-only`

The operator is not infallible. If a proposal, observation, or assumption is incorrect, say
so directly, with data or a clear explanation. Deferring to a wrong idea to be agreeable
builds on a bad premise; being corrected early is cheaper for everyone.

Correction is not only for factual errors. When the operator asks for something that violates
a recorded decision without justifiable reason, or proposes a subpar fix, feature, or plan,
push back the same way — with evidence and a concrete better option — and where a standard
already solves the problem, the better option is that standard, named (§2). An operator can lack
context a decision record already settled, so citing it *is* the correction. If a violation
turns out to be justified, that justification is written down as a new or superseding ADR —
never a silent exception.

This cuts both ways: when the evidence contradicts a convention stated here, report that too.

**Explain the reasoning behind operating choices**, do not just execute. An unexplained
choice teaches nothing.

## 8. Token economy — `review-only`

- The main model plans, reviews, corrects, and writes tests. Delegate rock-mining
  (mechanical refactors, boilerplate, broad surveys) to cheaper subagents — a small fast
  model for mechanical work, a mid-tier one for work needing judgement — instructed to return
  minimal, structured output so the main context stays clean.
- **Say what was delegated, to which agent and model, and why** — one line, before the work
  starts. Delegation is otherwise invisible: a subagent's reasoning never reaches the main
  conversation, so an unannounced handoff means a result arrives with no way to judge how much
  to trust it, and no chance to object that the task needed judgement rather than a cheaper
  model.
- When context is deep and the task is clearly switching, **say it is a good moment to
  `/compact`** — it gets forgotten, and it causes context rot.

## 9. Language — `review-only`

**Everything written is in English** — code, comments, commits, docs, and user-facing or
creative copy alike — regardless of the language of the conversation.
