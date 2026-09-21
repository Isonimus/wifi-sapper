---
id: '0050'
title: "Artifact privacy guard — redact on-air identifiers and block real BSSIDs from artifacts/"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0049: scrub the real third-party network identifiers already committed in the on-air
artifacts (and the slice/test prose that quoted them), and add a mechanical guard so a real BSSID can
never re-enter `artifacts/` unnoticed.

- **Scrub (one-time, whole history):** replace the nine real neighbour BSSIDs with
  `xx:xx:xx:xx:xx:xx`, the neighbour SSIDs (`<redacted-ssid>`, `<redacted-ssid>…`, `<redacted-ssid>`) with
  `<redacted-ssid>`, and the device's own MAC-derived `Sapper-XXXX` with `Sapper-XXXX`, everywhere they
  appear across all commits — done in the pre-remote window, before any push.
- **Guard (ongoing):** `scripts/check-artifact-privacy.mjs` fails on any hex MAC pattern under
  `artifacts/`, wired into `package.json` (`check:artifact-privacy`) and the pre-commit hook.
- **Invariant:** add `CLAUDE.md` §4 row 25 (ADR-0049), `verified_by` the guard for the BSSID half.

## Definition of Done

**Scenario A — a real BSSID in an artifact blocks the commit (tooling).**
- **Given** a file under `artifacts/` containing a hex MAC-address pattern (a real BSSID)
- **When** `check:artifact-privacy` runs (directly, or via the pre-commit hook)
- **Then** it exits non-zero and names the offending file, line, and MAC — the commit is blocked.
- **Proof:** demonstrated fail-before against the nine unscrubbed BSSIDs (exit 1, 11 occurrences).

**Scenario B — redacted evidence passes (tooling).**
- **Given** `artifacts/` where every BSSID is the non-hex placeholder `xx:xx:xx:xx:xx:xx`
- **When** the guard runs
- **Then** it exits zero — the placeholder is not hex, so it never matches.
- **Proof:** demonstrated pass-after against the scrubbed tree (exit 0).

**Scenario C — no real identifier survives anywhere in history (one-time scrub).**
- **Given** the pre-remote history that carried real BSSIDs/SSIDs in artifacts, two slice bodies, and a
  test comment
- **When** the identifiers are scrubbed across all commits and the tree is re-scanned
- **Then** no real identifier appears in any blob of any commit, and the immutable-body check still
  holds (a uniform global replacement changes no consecutive-commit line-gain relationship).
- **Proof:** `git grep` over `git rev-list --all` returns nothing for every real identifier; the
  pre-commit immutable check passes on this slice's commit.

## Verification

- **Tooling (`scripts/check-artifact-privacy.mjs`, wired `check:artifact-privacy` + pre-commit hook)** —
  Scenarios A and B. The guard runs on every commit against the real `artifacts/` tree; a real BSSID
  aborts the commit. This repo has no wired node-test lane (all suites are Unity C++; the
  `read-set.test.mjs` referenced by the hook/linter comments does not exist — logged in `LEDGER.md`),
  so the continuous hook exercise is the regression vehicle, and fail-before/pass-after was demonstrated
  by hand (recorded in `## As built`). A brittle dead unit test with no runner would be the
  "runs once" anti-pattern R11 forbids.
- **One-time scrub (`git filter-repo --replace-text`)** — Scenario C, verified by a whole-history grep.
- **Review** — the SSID half (a scanner cannot distinguish a real SSID from a placeholder) and the ADR
  rationale.

## As built

Shipped as designed (ADR-0049). No design changes during implementation.

- **Scrub:** `git filter-repo --replace-text` mapped the nine real BSSIDs → `xx:xx:xx:xx:xx:xx`, the
  three neighbour SSIDs → `<redacted-ssid>`, and `Sapper-XXXX` → `Sapper-XXXX`, across all 40 commits.
  A pre-scrub bundle backup was taken first. Post-scrub, a `git grep` over `git rev-list --all` for
  every identifier returned nothing; the affected artifacts show placeholders; slice-0007/0018 and the
  `test_wpasec_response` comment now carry the placeholders (the replacement was uniform across every
  commit's copy, so the immutable line-gain check is undisturbed).
- **Guard:** `scripts/check-artifact-privacy.mjs` scans `artifacts/` (skipping PNG/binary) for a hex
  MAC pattern and fails with the offending file/line/MAC. Fail-before was demonstrated on the
  unscrubbed tree (exit 1, 11 occurrences across `0016-hunt.loop.txt`); pass-after on the scrubbed tree
  (exit 0). Wired as `check:artifact-privacy` in `package.json` and invoked from `.claude/hooks/pre-commit`
  as a working-tree scan (ADR-0049 decision 4 — artifacts are wholesale-added, so the hook's
  commit-vs-tree machinery does not apply, and the scan stays a plain reusable `node` call outside the
  `$paths` list a read-set test holds equal to the linter scope).
- **Docs:** `CLAUDE.md` §4 gained row 25 (Source ADR-0049); `LEDGER.md` gained two items — verify-script
  source-side redaction, and the missing node-test lane / absent `read-set.test.mjs` (boyscout).
- **Enforcement scope:** BSSIDs mechanically (the geolocatable, highest-risk identifier); SSIDs
  review-only until the verify scripts redact at the source.
