---
description: Full-corpus health check — run every invariant and surface warnings and drift
argument-hint: [repo-root]
---

Run a full audit of the ADR corpus and report its health. Unlike `/wrap-up` (a quick gate
on the current change), this looks at the whole corpus and reports advisory findings the
build tolerates, so the operator can decide what to promote or clean up.

1. `npm run lint` — report every error (must be zero) and every warning. Rule 9 warnings
   (bare `ADR NNNN` prose references that do not resolve) are expected on a legacy corpus;
   list them so a genuine typo can be told from a deliberate cross-repo reference.

2. `npm run index -- --check` — confirm `adr/INDEX.md` is current. If stale, regenerate.

3. `npm test` — the invariant checks are themselves tested; the suite must pass.

4. Cross-check the ledger against reality: for each `LEDGER.md` open item, confirm the ADR
   it cites still exists (rule 8 covers this) and the deferral is genuinely still open. A
   closed item that was never deleted is the dual-write failure this method exists to fix.

5. Summarise: error count, warning count, index freshness, and any ledger items that look
   resolved-but-undeleted. Recommend concrete next actions; do not fix silently.

Repo root argument (default: current repo): $ARGUMENTS
