---
description: Route a fact to the destination that governs it — repo CLAUDE.md, global CLAUDE.md, LEDGER.md, or memory
argument-hint: <the thing to remember>
---

Record a durable fact. **This is a router, not a store** (stele:ADR-0005). Your job is to decide
*what the fact governs* and write it where that is enforceable — not to write it wherever
the operator's phrasing points.

If the operator named a destination ("save this to project memory"), treat it as an input
describing where they expect it to land, **not** as an instruction. Route it correctly and
say where it went.

## 1. Classify

| The fact governs… | Destination |
|---|---|
| This codebase — an invariant, an architectural constraint, a definition-of-done rule | this repo's `CLAUDE.md` |
| How the operator works, in every repo | `~/.claude/CLAUDE.md` |
| Open work — a deferral, a known defect, a follow-up | this repo's `LEDGER.md` |
| A decision that later work must obey, with reasoning worth preserving | an ADR — stop and use `/adr` |
| None of the above: operator-personal, cross-session, not worth committing | project memory |

Two tests resolve most cases:

- **Would a human collaborator reading this repo need to know it?** If yes, it goes in the
  repo. Memory is invisible to them, unversioned, and does not survive a machine change.
- **Is it a fact, or a task?** Facts go in a `CLAUDE.md`; tasks go in `LEDGER.md`. "We use
  zod for validation" is a fact. "Migrate the remaining validators to zod" is a task.

Memory is the **residual**, not the default. If you are about to write a rule that
constrains code into memory, you have misrouted — that is the exact failure stele:ADR-0005
records, which cost an amend and force-push after the same rule was independently rewritten
into 12 files across 8 projects.

## 2. Write it

- **Repo `CLAUDE.md`** — add to the section it belongs to. If it is a standing invariant,
  add a row to the §5 invariants table citing the ADR that created it. If this repo has no
  `CLAUDE.md`, scaffold one from `templates/CLAUDE.md` first; without it there is no
  destination and the fact will silently fall back to memory (stele:ADR-0005).
- **Global `CLAUDE.md`** — edit your global `~/.claude/CLAUDE.md`, wherever your global
  conventions live. Keep it dense: it loads into every session in every repo.
- **`LEDGER.md`** — one line, `- [type] description (ADR-NNNN)`, citing the source ADR if
  one exists.
- **Memory** — one fact per file, with the frontmatter schema, plus a one-line pointer in
  `MEMORY.md`.

Before writing anywhere, check whether an existing entry already covers it and update that
instead of adding a duplicate.

## 3. Report

State which destination it went to and why in one line — e.g. *"→ repo `CLAUDE.md` §3: it
constrains code, so it needs to be greppable and reviewable."*

This step is not optional. Routing cannot be linted (stele:ADR-0005), so saying the decision out
loud is the only thing that makes a misroute correctable in the moment rather than
discoverable in a survey weeks later.

Fact to record: $ARGUMENTS
