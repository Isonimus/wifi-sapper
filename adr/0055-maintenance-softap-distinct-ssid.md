---
id: '0055'
title: "The Maintenance SoftAP broadcasts a distinct SSID (Sapper-Maint-XXXX), not the provisioning name"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

The first-boot / STA-fail **provisioning** portal (ADR-0006) and the **Maintenance** results dashboard
(ADR-0039) both raise a SoftAP named `Sapper-XXXX` — `formatSoftApSsid()` derives the same name from the
SoftAP MAC for both, and ADR-0039 deliberately reused it (`maintenance_portal.cpp`: "the same
`Sapper-XXXX` identity the README documents").

Field experience this session proved that reuse a UX hazard. A provisioned device whose STA could not
associate (a network out of range, or a mistyped credential) correctly fell back to the provisioning
portal after its retry budget (ADR-0006 #3) — which raised `Sapper-XXXX`. The operator, expecting
Maintenance, saw `Sapper-XXXX` appear and concluded the device "enters Maintenance every time,"
because the two SoftAPs are **indistinguishable by name**. They are in fact entirely different: one
serves the setup *form*, the other the recovered-PSK *dashboard*; one is an automatic fallback, the
other a deliberate BOOT-press entry (ADR-0053). A shared identity for two modes that differ in both
purpose and how you reach them is exactly the ambiguity that cost a debugging session here.

This is the `~/.claude/CLAUDE.md` §2 case: ADR-0039's name-reuse was a reasonable default (one fewer
name to document) that field use showed to be wrong. Recorded as a decision that supersedes only that
clause, not the rest of ADR-0039.

## Decision

**The Maintenance SoftAP broadcasts `Sapper-Maint-XXXX`; the provisioning portal keeps `Sapper-XXXX`
unchanged.** The `Maint` tag makes the mode self-evident from the Wi-Fi list, and the *absence* of the
tag tells an operator expecting Maintenance that a `Sapper-XXXX` they see is the setup fallback, not
Maintenance.

1. **A dedicated pure formatter.** `formatMaintenanceApSsid()` (in `net/provisioning`, beside
   `formatSoftApSsid`) writes `Sapper-Maint-%02X%02X` from the last two SoftAP-MAC bytes into a
   compile-time-sized buffer (`kMaintApSsidBufSize`), mirroring `formatSoftApSsid`'s no-truncation
   contract. Host-tested. The MAC suffix is kept — it disambiguates *units* (ADR-0006 #5's reason),
   which the mode tag does not replace.

2. **Provisioning is untouched — minimal blast radius.** `formatSoftApSsid()`, `kSoftApSsidBufSize`,
   ADR-0006 #5, the first-boot README guide, and `artifacts/0007-*.state.txt` all keep `Sapper-XXXX`.
   Only the Maintenance path changes. (The alternative — tagging both as `Sapper-Setup-XXXX` /
   `Sapper-Maint-XXXX` — is more symmetric but supersedes ADR-0006 #5 and touches the provisioning
   tests, first-boot docs, and two verify scripts; rejected as more churn than the fix needs.)

3. **Decision-level supersession of ADR-0039's name-reuse only.** ADR-0039 stays `accepted`; its
   dashboard, PSK, SoftAP-hardening, panel, and control decisions are unchanged. Per the
   ADR-0027→ADR-0029 / ADR-0053 precedent, this is recorded in prose and the §4 table, not as
   frontmatter supersession.

4. **Extends ADR-0049 #25 (the artifact-privacy guard).** The guard matched only `Sapper-[hex]{4}`; a
   Maintenance SSID leaked into a committed artifact would now read `Sapper-Maint-[hex]{4}` and slip
   past. The guard gains that pattern, and the documented redaction placeholder gains `Sapper-Maint-XXXX`
   (non-hex, so it never self-matches — the same property that makes `Sapper-XXXX` a safe placeholder).

## Consequences

- **§4 #25 is updated in this commit** to cite ADR-0055, add the `Sapper-Maint-XXXX` placeholder, and
  note the guard now matches the tagged form. The guard regex is extended accordingly.
- **New pure unit + test:** `formatMaintenanceApSsid` + a `test/test_provisioning` case
  (`Sapper-Maint-ABCD` from a known MAC), fail-before/pass-after against the old shared-name behaviour.
- **Live docs updated in the same change:** README's Maintenance section (no longer "same name as the
  setup portal"), the `verify:maintenance` table row, `scripts/0040-maintenance-verify.mjs`'s
  join-the-AP comment, and the `maintenance_portal.cpp` identity comment.
- **The on-device Maintenance panel** (`drawMaintenanceScreen`) shows the new SSID automatically — it
  renders `apSsid()`, which now returns the tagged name; the `AP: <ssid>` line still fits its buffer.
- **Does not change** the provisioning SSID, ADR-0006 #5, or `formatSoftApSsid`. No other ADR's
  reasoning changed.
- Closes the SSID-collision UX hazard this session surfaced; no LEDGER item is left for it (found and
  fixed together).
