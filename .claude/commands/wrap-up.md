---
description: End-of-task gate — run the checks and ask the four questions that get forgotten
---

Run before finishing a task. The point is to make mechanical what memory keeps dropping:
the linter catches structural drift, and four questions catch the follow-ups that never
get recorded until they have gone stale (stele:ADR-0003).

1. Run the linter and the index check:
   - `npm run lint` — must be green (rule 9 warnings are acceptable on a legacy corpus).
   - `npm run index` — regenerate `adr/INDEX.md` and stage it if it changed. A stale
     generated index is drift by another name.
   - `npm test` — the test suite must pass; docs changes must not disturb code.

2. **Mutation check** — only if this repo has `scripts/check-mutants.mjs` *and* this change
   touched a module its list covers. Otherwise skip it and say so; on most changes it has
   nothing to look at.

   - `npm run mutants`. Every non-exempt mutant must die.
   - A **survivor is not a bug** — it is correct behaviour that no test is watching, so a
     later refactor could reverse it in silence. Fix it by writing the missing regression
     test, never by deleting the mutant.
   - Mark a mutant `equivalent` only when the mutated code genuinely means the same thing,
     and say why in the entry. That field is the one way a survivor passes, so it is also
     the obvious place to bury an inconvenient gap.
   - It measures whether tests *bite*, not whether code is right: it cannot see a missing
     input or a rule that was wrong from the start. A green run is not a correctness claim,
     and step 3 is what covers what it misses.

3. **Adversarial pass** (stele:ADR-0017) — run it if this change touched a `CLAUDE.md` §4
   standing invariant, an exported/public API, a data format or anything persisted, or a
   Definition of Done scenario the slice flagged as risky. Otherwise skip it and say so.

   - Spawn one subagent (two only if the second gets a *different* brief: **correctness** —
     edge cases, error paths, boundaries, ordering and partial-failure hazards; **cost** —
     complexity class, allocation, IO in loops). Sonnet 5 for correctness; announce the
     delegation before it starts (`~/.claude/CLAUDE.md` §6).
   - **Tell the reviewer to read the files itself and not to spawn subagents of its own.** A
     brief that fans out and does not forbid recursion has unbounded cost: a 12-reviewer sweep
     on 2026-07-30 became an estimated 40–50 agents, exhausted a month's API budget, and lost
     eight batches mid-run — while the one reviewer that read its own files returned the best
     report of the twelve. One reviewer deciding to parallelise is enough to reproduce that at
     smaller scale, so the sentence belongs in every brief, not only the wide ones
     (stele:ADR-0017).
   - Brief it **blind to intent, aware of law**: give it the diff, this repo's `CLAUDE.md`,
     and `adr/INDEX.md`. Do *not* give it the conversation, your rationale, or the slice's
     claims about itself. A reviewer handed the reasoning returns the reasoning; one handed
     nothing flags every deliberate deviation as a bug.
   - Require a concrete failure scenario per finding — input or state → wrong output,
     crash, or a counted cost. Reject "consider validating this" at intake, uninvestigated.
   - **Reproduce before acting.** An accepted correctness finding becomes a test that fails
     before the fix and passes after; one you cannot reproduce is rejected. The reviewer had
     no intent context and is confidently wrong a predictable share of the time.
   - Route what survives through the four questions below — fix, `LEDGER.md`, `/adr`, or
     `/slice`. **And record the rejections**: a finding you correctly dismissed will be
     raised again by every future fresh reader until the reason is written at the site
     (stele:ADR-0012) or in the §4 table. That is what makes the next pass cheaper than this one.

4. Then answer these four out loud, and act on each:
   - **Did this change a user- or dev-facing API or feature?** If so, update `README.md`
     (and any docs) in the same change — it is a live document. A **hand-maintained**
     `CHANGELOG.md` is a live doc too and gets its entry now. A **generated** one — built
     from tags, as in the repo that ships this kit (stele:ADR-0026) — is never hand-edited:
     regenerating it is a release step, and an edit here fails that repo's own `--check`.
   - **Did this make a decision later work must obey?** If so, write it with `/adr` now,
     while the reasoning is fresh. An unrecorded decision is re-litigated later from
     nobody's memory.
   - **Did this defer something** — a follow-up, a known-but-unfixed bug, a TODO? If so,
     add one line to `LEDGER.md` citing the relevant ADR. The ledger is the only place
     deferrals live; a TODO in code or a note in your head is not tracked.
   - **Did this write code a later operator would plausibly try to "fix"?** — a deliberate
     deviation, a non-obvious constraint, a hard-won exception. If so, cite the governing
     ADR at that site in a comment (stele:ADR-0012), so the choice announces it is on purpose
     where the edit happens, not only in the §4 table nobody thinks to open.

5. Report what you found and did for each of the four, so the operator can confirm
   nothing was silently skipped.
