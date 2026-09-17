---
description: Install the method kit (CLAUDE.md, LEDGER.md, linter, index, pre-commit hook) into a git repo
argument-hint: [repo-root] [--check | --update [--force]]
---

Install this kit into a repo, or verify an existing install (stele:ADR-0006). The script does
the mechanical half; you do the two halves that need judgement — filling the scaffolded
`CLAUDE.md`, and wiring verify scripts.

Target repo: `$ARGUMENTS` (default: the current repo root).

## 1. Look before writing

Run the dry run first and read what it plans:

```
node scripts/init-method.mjs <target>
```

It writes nothing. `WOULD` lines are the plan, `KEEP` means a file already exists and
will be left alone, `PROBLEM` means something needs a decision. Report the plan to the
operator before applying it if anything is being scaffolded over a repo that already has
conventions of its own.

If the target is not a git repository the run refuses. Offer `git init`; do not work
around it — the hook has nowhere to live.

## 2. Apply

```
node scripts/init-method.mjs <target> --apply
```

This scaffolds `adr/`, `CLAUDE.md` and `LEDGER.md` (never overwriting), vendors
`lint-docs.mjs` / `build-index.mjs` / the hook and the slash commands, generates
`adr/INDEX.md`, and installs the pre-commit hook **only if the corpus lints clean**.

Nothing outside the target repo is written — never `$HOME`, never `~/.claude/CLAUDE.md`
(stele:ADR-0016). Wiring the operator's machine-level conventions is a personal choice, not
an install step; do not add it back as a convenience.

The commands and `docs/quality-bar.md` are vendored under softer rules than the machinery
(stele:ADR-0023): a repo may edit its own copy of `/slice`, `/wrap-up` or the bar to say
something repo-specific, and an install keeps that edit rather than overwriting it. If you
edit one, say so — an edit made in an installed repo does not travel back to the toolkit.

The quality bar is the standard the slice `## Definition of Done` is measured against
(stele:ADR-0024). Cite it from the scaffolded `CLAUDE.md`; do not restate its rules there.

## 3. If the repo already has a hook framework

A target with a `.pre-commit-config.yaml` gets the doc checks **composed into it** as a
`repo: local` block rather than a symlink (stele:ADR-0008) — the framework owns
`.git/hooks/pre-commit`, and a symlink there is silently erased by the next
`pre-commit install`. The append is idempotent and additive; the existing config is
never reordered or rewritten, and `--update` leaves it alone.

Two things to watch:

- A config the script does not recognise (no top-level `repos:`) is **refused**, not
  guessed at. Add the block by hand and say you did.
- `--check` reports a problem when the framework is configured but never installed. That
  is not our hook failing — it means *no* hook runs, including the repo's own ruff and
  mypy. Tell the operator to run `pre-commit install`; do not paper over it by
  symlinking ours instead.

## 4. If it refused the hook

A refusal is normal on a repo that already has work in it, and the reason is almost
always **rule 11: a `scripts/*-verify.mjs` that no `package.json` command runs**. An
unwired verify script ran once, the day it was written; the linter treats that as an
error, so the corpus is red and a hook would block every commit.

Fix it, in this order — the order is the whole point:

1. Add one `"verify:<name>": "node scripts/<name>-verify.mjs"` per unwired script. Use
   the name a human would recognise, not the filename verbatim. A `verify:all` chaining
   the headless ones is worth adding; leave out any script that needs a human watching
   or listening, and say so in a comment or in `CLAUDE.md`.
2. Fix any other lint errors (dangling supersessions, bad frontmatter). Never silence a
   rule to get green.
3. Re-run `--apply`. The hook installs.

Do **not** edit the target's `package.json` without telling the operator what you added.

## 5. Finish the scaffold

If `CLAUDE.md` was newly scaffolded it contains `{{PLACEHOLDER}}` fields. Fill them from
the repo itself — read the README, the manifest, the source layout. Do not invent a
project description. Then delete the sections that describe machinery the repo does not
have: a verification-harness section in a repo with no `scripts/` states rules about
files that do not exist, which makes the file false on arrival.

## 6. Verify, and say what happened

```
node scripts/init-method.mjs <target> --check
```

Writes nothing; fails on a missing or broken hook, a drifted vendored **script**, a stale
index, or a red corpus. Report its output verbatim rather than summarising it as
"installed".

`LOCAL` and `MISSING` lines are about commands only and are **not** failures — they say
this repo adapted or declined one. Read them, mention them, do not "fix" them without
asking; that is somebody's deliberate edit.

A `PROBLEM` on a command is different: it means that file is **behind the toolkit and
unmodified here**, so the repo is simply missing a fix. `--update` takes it.

## 7. Updating an installed repo

```
node scripts/init-method.mjs <target> --update --apply
```

`--update` re-copies the vendored scripts and the hook unconditionally — a locally edited
linter is a defect, not an adaptation (stele:ADR-0006). Commands are gentler
(stele:ADR-0023): anything this repo has **not** touched takes the toolkit's version,
anything it **has** touched is kept and reported.

`.claude/.stele-vendored.json` is what makes that distinction possible — it records what
the toolkit last wrote, so a stale copy and a deliberate edit stop being the same
observation. **Commit it.** A repo without one (installed before the record existed) has
every differing command reported as unreconciled and overwrites none of them; reconcile
those by hand, once.

`--update --force` discards local adaptations. Never reach for it to make output tidy —
it is for when the operator has decided an adaptation should go. Say what will be lost
before running it.
