---
id: '0049'
title: "On-air verify artifacts are scrubbed of third-party network identifiers before commit"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

ADR-0004 made verify artifacts committed evidence: a `scripts/<slice>-verify.mjs` writes screenshots,
dumps, and measured numbers to `artifacts/`, and those files are committed so a reviewer can see the
slice worked. That decision was made before any artifact was produced *on air*. It did not anticipate
that a real hunt or upload run records the identifiers of networks the operator does **not** own.

Preparing the repo for a public remote surfaced the gap. The committed evidence contained real
third-party network identifiers:

- `artifacts/0016-hunt.loop.txt` — nine real neighbour **BSSIDs** and two SSIDs (`<redacted-ssid>`, an
  `<redacted-ssid>…` printer) swept up during channel-hopping discovery.
- `artifacts/0018-upload.drain.txt` — a neighbour **SSID** (`<redacted-ssid>`) whose handshake was
  captured and uploaded to wpa-sec.
- The same neighbour SSID and the device's own MAC-derived SoftAP name (`Sapper-XXXX`) had also
  reached two immutable slice bodies and a test comment.

A BSSID is a MAC address, and a MAC address is geolocatable through public Wi-Fi databases such as
WiGLE: publishing one pinpoints where the device ran and exposes a bystander's network. This clears
the *letter* of the existing rule ("never commit real pcaps or cracked PSKs" — these are neither) but
not its intent, and for a public repository about deauth-capable tooling it reads as recon of specific
real networks. The existing rule guarded the two highest-value leaks (capture bytes, recovered
passwords) and missed the identifiers that ride the *text* evidence.

This is the `~/.claude/CLAUDE.md` §2 case: a recorded decision (ADR-0004) that lacked context a later
situation supplied. It is recorded as a decision, not taken as a silent exception.

## Decision

**On-air verify artifacts carry no real third-party network identifier. Redacted evidence uses a
placeholder, and the absence of real BSSIDs from `artifacts/` is mechanically enforced.**

1. **Redaction, not removal.** The evidentiary value of an on-air artifact is the *structure* — a
   capture happened, N APs were discovered, a real 4-way handshake reconstructed and was accepted —
   not the specific SSID/BSSID. So identifiers are replaced with a placeholder, keeping the artifact
   as evidence: a BSSID becomes `xx:xx:xx:xx:xx:xx`, an SSID becomes `<redacted-ssid>`, and the
   device's own MAC-derived SoftAP name becomes its documented generic form `Sapper-XXXX`. The
   already-committed identifiers were scrubbed from all history in the same pre-remote window
   (slice-0050 `## As built`), before any push.

2. **BSSID absence is mechanically enforced.** `scripts/check-artifact-privacy.mjs` fails if any file
   under `artifacts/` contains a hex MAC-address pattern. The placeholder is deliberately non-hex, so
   it never matches. The guard is wired into `package.json` (`check:artifact-privacy`) **and the
   pre-commit hook**, so it runs on every commit — not once. A MAC is a fixed, unambiguous pattern, so
   this half needs no human judgement; the synthetic fixture MACs in `src/`/`test/`
   (`aa:bb:cc:dd:ee:ff` and friends) are out of scope because the guard scans `artifacts/` only, where
   nothing but generated evidence lives.

3. **SSID redaction is review-only, and the durable fix is at the source.** An arbitrary SSID string
   is not machine-distinguishable from a placeholder, so a scanner cannot enforce it without a brittle
   allowlist; `/wrap-up` review is the enforcement. The real fix is to redact *in the verify scripts
   themselves*, before they ever write an artifact — so a real identifier never reaches disk. That is a
   change across the on-air `scripts/*-verify.mjs` and is deferred to a `LEDGER.md` item citing this
   ADR.

4. **The pre-commit scan reads the working tree, deliberately.** The stele hook otherwise grades the
   *commit* (`git write-tree`, stele:ADR-0018) to defeat a staged-clean/unstaged-broken split. That
   split is a real hazard for hand-edited docs; it is not one for artifacts, which are wholesale
   machine-written dumps added as a unit. So the artifact scan stays a plain `node` invocation reused
   verbatim by `npm run` and a future CI lane, rather than being threaded through the hook's `$paths`
   extraction (which a read-set test holds equal to the linter's scope — a scope artifacts do not
   belong in).

## Consequences

- A new `CLAUDE.md` §4 standing invariant (row 25) is added in this commit, citing this ADR:
  `verified_by: scripts/check-artifact-privacy.mjs` for the BSSID half, `review-only` for SSIDs,
  `pending (LEDGER)` for verify-script-side redaction.
- `scripts/check-artifact-privacy.mjs` is wired into `package.json` and `.claude/hooks/pre-commit`.
  Its regression behaviour was demonstrated fail-before (exit 1 on the nine real BSSIDs) / pass-after
  (exit 0 on the scrubbed placeholders); the hook running it on every commit is the continuous
  exercise, since this repo has no wired node-test lane (see the LEDGER note on the absent
  `read-set.test.mjs`).
- **Extends ADR-0004; supersedes nothing.** ADR-0004's "commit artifacts as evidence" stays true — the
  artifacts are still committed; they are redacted first. No mind was changed, so no supersession.
- **Deferred, not built here:** verify-script-side redaction (redact identifiers before writing the
  artifact) and a wired node-test lane for `scripts/*.mjs` guards. Both recorded in `LEDGER.md`.

## Amendment — 2026-09-21: corrections from slice-0050's adversarial review

slice-0050's blind adversarial pass (the `/wrap-up` step for this §4 change) found real defects in
the guard as first shipped. The corrections, applied in `fix(slice-0050)`:

- **Decision 4 is reversed — the pre-commit scan reads the committed tree, not the working tree.** The
  claim that "artifacts are wholesale dumps, so the staged-clean/unstaged-broken split does not arise"
  was an assumption about *how* artifacts are authored, not a guarantee. A plain
  `sed`-then-forget-to-`git add` (reproduced: stage a real BSSID, scrub the working copy, commit)
  committed the real identifier while the working-tree scan passed. The hook now extracts `artifacts/`
  from `$tree` and scans that, exactly as it does for the three stele checks (stele:ADR-0018) — the
  original reasoning for the working-tree scan was wrong.

- **PNG/binary artifacts are an explicit, unenforceable exception.** The HUD/toast/splash verify
  scripts screenshot the panel, and the panel renders the target SSID/BSSID as *pixels* — which no
  regex can read, so the guard skips PNGs. The already-committed PNGs were checked and render only
  synthetic identifiers (the probes inject `SAPPER-VERIFY`/`SAPPER`, no network). The durable fix is
  redaction at the verify-script source (feed placeholder identifiers into the surface before
  capture), which the deferred LEDGER item now names PNGs for. Row 25's mechanical guarantee is
  therefore scoped to **text** artifacts; PNG identifiers are review-only until source redaction lands.

- **The MAC-derived SoftAP name and non-colon MAC formats are now caught (decision 2 generalized).**
  The device SoftAP name `Sapper-%02X%02X` (four hex digits, no colons) was invisible to a
  colon-only MAC regex, and hyphen/dotted MAC notations bypassed it. The guard now also matches
  `Sapper-[0-9A-Fa-f]{4}` and hyphen/Cisco-dotted MACs; the non-hex placeholders never match, so no
  false positive.

- **No CI backstop exists yet, and the guard is hook-only.** The hook's inherited "CI is the backstop"
  comment was false — no workflow is wired — so a clone that skips `/init-method` is unguarded. The
  comment is corrected and the LEDGER CI item now calls for the CI lane to run this guard.

- **This ADR's one-time history scrub broke ADR-0019 for two slices, accepted as a justified
  exception.** The `filter-repo --replace-text` scrub rewrote `<redacted-ssid>` → `<redacted-ssid>`
  inside the frozen bodies of slice-0007 and slice-0018 — a line *changed*, which ADR-0019 forbids and
  `check-immutable.mjs` would reject for a normal commit. A history rewrite replaces every commit's
  copy at once, outside commit/hook flow, so the check cannot see it; slice-0050's "the line-gain
  check is undisturbed" is true only in that sense, not that the rule was honored. Per
  `~/.claude/CLAUDE.md` §2 this is **recorded as an accepted, one-time exception** rather than asserted
  away: not publishing a bystander's captured SSID outranks body-immutability for a pre-publish scrub,
  and the redaction preserves each body's meaning (a real handshake was captured and accepted). No
  future in-repo edit may rewrite an immutable body; this exception is the scrub alone.

## Amendment 2 — 2026-09-21: second adversarial review (fix(slice-0050), second round)

A second blind adversarial pass on the hardened guard found more, all fixed in a second
`fix(slice-0050)`:

- **This ADR and slice-0050 quoted the real identifiers in their own prose (F1, critical).** The
  Context above and Amendment 1 named the specific neighbour SSIDs and the device's real SoftAP name
  to *describe* the scrub — putting the exact identifiers this ADR exists to remove into two immutable
  files headed for a public remote. The guard only scans `artifacts/`, so it never saw them. Fixed by a
  second one-time `filter-repo --replace-text` pass redacting those literals to the standard
  placeholders across all history (another accepted immutability exception, same rationale as
  Amendment 1). Consequence: the lines that named the mapping now read circularly
  (`<redacted-ssid>` → `<redacted-ssid>`) — cosmetic, the meaning is unchanged and the real values are
  gone. A *mechanical* corpus scan of `adr/`+`slices/` is **not** viable, because the corpus
  legitimately holds synthetic example MACs (`aa:bb:cc:dd:ee:ff`, `Sapper-0000`) a scanner cannot tell
  from a real one — so keeping real identifiers out of ADR/slice prose is a **review discipline**, now
  stated in the guard header and enforced at `/wrap-up`, not a check.

- **The guard failed open on a walk error (F2, high).** A dangling symlink deep in `artifacts/` threw
  inside `walk()`, which a `try/catch` meant only for an absent top-level directory swallowed — the
  scan then printed "ok" and passed a real BSSID. Now only a genuinely-absent `artifacts/` passes;
  every other walk/read error propagates and fails the commit closed.

- **Raw capture files were neither skipped nor scannable (F3, high).** `.pcap` bytes are binary, so the
  text patterns never matched, and `scripts/0028-deauth-verify.mjs` writes a real forced-handshake pcap
  into `artifacts/`. Per §4 invariant #9 capture bytes belong only behind the `CaptureSink` seam, never
  in committed evidence, so the guard now **hard-fails** on any `.pcap`/`.pcapng`/`.cap` under
  `artifacts/`, and `.gitignore` excludes them.

- **A pathspec-limited commit bypasses the index-based hook (F4, high, structural).** `git commit --
  <path>` (and `--only`) commits *working-tree* content for the named path, while the hook's
  `git write-tree` reflects the *index* — so the tree scanned is not the tree that lands. This is a
  property of the stele pre-commit model (stele:ADR-0018) shared by all four checks, not unique to this
  guard, and cannot be closed inside `pre-commit`. The durable backstop is a CI (or `pre-push`) lane
  that scans the actually-pushed tree; recorded in the LEDGER.

- **Minor:** the hook's stale "CI still catches it" bypass note is corrected to "nothing else catches
  it yet" (F5); the hook now existence-checks the guard before invoking it, failing closed with a clear
  message (F7); and a separator-less 12-hex MAC (`aabbccddeeff`, the capture-filename format) is a
  known latent gap left unmatched to avoid false-positiving on hashes — reachable only via a pcap
  filename text-dumped into an artifact, and pcaps now hard-fail regardless (F6, LEDGER).
