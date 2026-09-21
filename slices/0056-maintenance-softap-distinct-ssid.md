---
id: '0056'
title: "Maintenance SoftAP distinct SSID — Sapper-Maint-XXXX, so it can't be mistaken for the setup portal"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0055: the Maintenance SoftAP broadcasts `Sapper-Maint-XXXX` instead of the provisioning
portal's `Sapper-XXXX`, so a device that fell back to provisioning (STA-fail) can never be mistaken for
Maintenance. Provisioning is untouched.

- **Pure formatter (`net/provisioning`):** `formatMaintenanceApSsid()` + `kMaintApSsidBufSize`, writing
  `Sapper-Maint-%02X%02X`; host-tested, compile-time-sized (no truncation), mirroring `formatSoftApSsid`.
- **`MaintenancePortal`:** widen `m_ssid` to `kMaintApSsidBufSize` and call the new formatter.
- **Artifact guard (ADR-0049 #25):** match `Sapper-Maint-[hex]{4}` too; document the `Sapper-Maint-XXXX`
  placeholder.
- **Docs:** README maintenance section + `verify:maintenance` row, `scripts/0040-maintenance-verify.mjs`
  comment, `CLAUDE.md` §4 #25.

## Definition of Done

**Scenario A — the Maintenance SSID is tagged and distinct (pure).**
- **Given** a known SoftAP MAC ending `AB:CD`
- **When** `formatMaintenanceApSsid(mac, out)` runs
- **Then** `out` is exactly `Sapper-Maint-ABCD`, and `formatSoftApSsid(mac, ...)` still yields
  `Sapper-XXXX` (`Sapper-ABCD`) — the two are distinguishable.
- **Proof:** `test/test_provisioning` (new case beside the existing `formatSoftApSsid` cases) — fails
  before (no such function / shared name) and passes after. See `## As built`.

**Scenario B — the artifact guard catches a leaked tagged SSID (tooling).**
- **Given** a text artifact under `artifacts/` containing `Sapper-Maint-1A2B`
- **When** `npm run check:artifact-privacy` runs
- **Then** the guard fails on it as a SoftAP-name identifier (as it already does for `Sapper-1A2B`),
  while the `Sapper-Maint-XXXX` placeholder passes (non-hex).
- **Proof:** demonstrated by hand against the guard (no node-test lane, LEDGER audit item), the same
  regression vehicle slice-0050/0052 used. See `## As built`.

**Scenario C — the device broadcasts the tagged SSID on air (review + on-device).**
- **Given** a device entering Maintenance
- **When** `MaintenancePortal::begin()` raises the SoftAP
- **Then** the broadcast SSID and the panel `AP:` line read `Sapper-Maint-XXXX`.
- **Proof:** `scripts/0040-maintenance-verify.mjs` (join the `Sapper-Maint-XXXX` AP); the `begin()`
  wiring is review-only. See `## As built`.

## Verification

- **Pure formatter (Scenario A)** — `pio test -e native` (`test_provisioning`), fail-before/pass-after
  in `## As built`.
- **Artifact guard (Scenario B)** — by-hand fail-before/pass-after against `check-artifact-privacy.mjs`
  (no wired node-test lane; LEDGER audit item), plus CI re-runs the guard on every push.
- **On-air SSID (Scenario C)** — `scripts/0040-maintenance-verify.mjs`; `begin()` wiring review-only.

## As built

Shipped as designed (ADR-0055), in one commit with the ADR, the §4 #25 update, and the guard change.

- **Pure formatter (Scenario A).** `formatMaintenanceApSsid()` (`net/provisioning`) writes
  `Sapper-Maint-%02X%02X` into a `kMaintApSsidBufSize` (18) buffer, beside `formatSoftApSsid` (unchanged).
  One extra snprintf, not a shared-prefix helper — the second instance, not the third (§3). Fail-before/
  pass-after: reverting it to the shared `Sapper-%02X%02X` made `test_maintenance_softap_ssid_is_tagged_and_distinct`
  **FAIL** (`Expected 'Sapper-Maint-ABCD' Was 'Sapper-ABCD'`); the tag makes it **pass** (272 native cases,
  up from 271). The test also pins that `formatSoftApSsid` still yields `Sapper-ABCD` and that the two differ.
- **Device wiring (Scenario C).** `MaintenancePortal::begin()` calls `formatMaintenanceApSsid` into a
  widened `m_ssid[kMaintApSsidBufSize]`; the SoftAP and the panel `AP:` line now read `Sapper-Maint-XXXX`.
  Review-only glue.
- **Artifact guard (Scenario B).** `check-artifact-privacy.mjs` gained a `Sapper-Maint-[hex]{4}` pattern
  (the old `Sapper-[hex]{4}` did not match the tagged form). Demonstrated by hand: a planted
  `artifacts/_ssidtest.txt` with `Sapper-Maint-1A2B` failed the guard (`[Maintenance SoftAP name]`), while
  the `Sapper-Maint-XXXX` placeholder passed (non-hex).
- **Docs & wiring.** `CLAUDE.md` §4 #25 cites ADR-0055 + the new placeholder and tagged pattern; README's
  Maintenance section no longer says "same name as the setup portal"; the `verify:maintenance` row and
  `scripts/0040-maintenance-verify.mjs` name the `Sapper-Maint-XXXX` AP. Provisioning, `formatSoftApSsid`,
  ADR-0006 #5, and `artifacts/0007-*.state.txt` are untouched.
- **Builds.** `pio test -e native` 272/272; `pio run -e cardputer` SUCCESS; `npm run lint` 0/0; cppcheck exit 0.
