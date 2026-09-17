---
description: Scaffold a new slice (one feature work-unit) with frontmatter pre-filled
argument-hint: <short title of the feature>
---

Create a new **slice**: one feature work-unit, written *before* implementation and frozen
at merge (stele:ADR-0001). A slice is a current-state claim while open, so it goes stale by
nature — freezing it to past tense at merge ("this is what shipped") converts it into a
historical claim that never goes stale. If you are recording a durable decision rather
than a unit of feature work, use `/adr` instead.

Steps:

1. Compute the next free id: highest ordinal in `adr/` (and `slices/` if present) plus
   one, zero-padded to four digits.
2. Scan `LEDGER.md` for open items this slice touches — a `[feature]` it delivers, a
   `[bug]` it fixes, a `[deferred]` it finally picks up. Fold each relevant one into
   `## Goal` so the slice knows the history it is answering, and **delete the line from the
   ledger for any item this slice closes** — closing an item means deleting its line (see
   the ledger's own header). If nothing relates, note nothing; this is a read, not a
   requirement to invent links.
3. Write the file opening with the stele:ADR-0002 frontmatter block, `type: slice`:

   ```
   ---
   id: 'NNNN'
   title: "<title>"
   type: slice
   status: accepted
   date: <today, YYYY-MM-DD>
   supersedes: []
   superseded_by: []
   ---
   ```

4. Body sections, written before you code:
   - `## Goal` — what ships and why, in one paragraph. Fold in any ledger items from step 2.
   - `## Definition of Done` — **required.** The acceptance criteria as one or more
     **Given / When / Then** scenarios, written before you code (stele:ADR-0011). This is the
     grammar, not a toolchain: plain markdown steps, no Cucumber. State the observable
     outcome — a criterion with no observable "Then" is not done-able. Each scenario's
     proof is named below in `## Verification`; a scenario with no proof is an unmet
     criterion, not a finished one. Example:

     ```
     ## Definition of Done

     - **Given** a fresh world
     - **When** the player places the block
     - **Then** it renders with its mesher shape, a hotbar icon, and a hand-held model
     ```

   - `## Design` — the approach; cite measured numbers from a probe where a design
     question has a measurable answer, not estimates. State **what existing code was
     considered**: what you searched for, what you reused, and what you deliberately did
     *not* reuse and why (stele:ADR-0013). This is not a mandate to reuse — the wrong abstraction
     costs more than a duplication (global §3, rule of three); it makes the
     reuse-or-duplicate choice *visible*, so an un-considered duplication is caught at
     review. It is checked by human read-through, not the linter: whether you truly searched
     is the coverage question no machine decides.
   - `## Verification` — **required.** Name the unit tests, and if the behaviour cannot be
     asserted in a unit test (rendering, worldgen, physics, timing), name the
     `scripts/<slice>-verify.mjs` script and confirm it is wired into `package.json`
     (stele:ADR-0004). A slice with no `## Verification` section is incomplete. The linter
     enforces both required sections (R12/R13): a `type: slice` must carry `## Verification`
     and a `## Definition of Done` holding at least one full Given/When/Then triad.
   - `## As built` — filled in at merge: what actually shipped, in past tense, confirming
     each Definition-of-Done scenario was met or recording the deviation. This is the
     freeze step.
5. Regenerate the index and run the linter; both green before done.

Title argument: $ARGUMENTS
