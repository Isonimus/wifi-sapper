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
