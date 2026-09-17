---
description: Scaffold a new Architecture Decision Record with frontmatter pre-filled
argument-hint: <short title of the decision>
---

Create a new ADR recording a decision that later work must obey — a mechanism, data
format, or boundary. An ADR asserts *"on date X we chose Y because Z"*: a historical
claim that stays true forever (stele:ADR-0001). If you are recording a **feature work-unit**
rather than a durable decision, use `/slice` instead.

Steps:

1. Compute the next free id: the highest ordinal across **both** `adr/` and `slices/` plus
   one, zero-padded to four digits. ADRs and slices share one id space — rule 2 fails on a
   duplicate id whichever directory it sits in. Do not reuse or renumber.
2. Choose the `type`: `architecture` for a durable decision, `slice` for a feature unit,
   `batch` only if it genuinely bundles several unrelated decisions that cannot map 1:1.
3. Write `adr/NNNN-<kebab-title>.md` opening with the stele:ADR-0002 frontmatter block:

   ```
   ---
   id: 'NNNN'
   title: "<title>"
   type: architecture
   status: accepted
   date: <today, YYYY-MM-DD>
   supersedes: []
   superseded_by: []
   ---
   ```

4. Body sections: `## Context` (the forces and the observed problem — cite data, not
   estimates), `## Decision`, `## Consequences`.
5. If this decision **supersedes** an existing ADR: add its id to this ADR's `supersedes`,
   set the old ADR's `status: superseded` and add this id to its `superseded_by`, and in
   the body state *why the old reasoning was wrong* — that record is the point. Both
   directions must match or rule 4 fails. Never edit the old ADR's prose body; only its
   status/supersession fields may change.
6. If the decision defers something, record it as one line in `LEDGER.md` citing this ADR.
   Do not track it anywhere else.
7. Regenerate the index (`npm run index`) and run the linter (`npm run lint`) — both must
   be green before you are done.

Title argument: $ARGUMENTS
