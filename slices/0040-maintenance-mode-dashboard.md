---
id: '0040'
title: "Maintenance mode: BOOT-hold into a hardened-SoftAP dashboard of persisted results"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Ship the first half of ADR-0039: a **Maintenance boot phase** the operator enters by **holding
BOOT at power-on**, which raises the SoftAP under a **dedicated maintenance passphrase** and
serves a **read-only web dashboard** of persisted state — recovered passwords (plaintext PSKs,
by ADR-0039 decision 5), capture-queue depth, deauth-arm state, and whether the device has ever
synced — read through the existing `CrackedManifest`/`CrackedStore` (#12) and `loadProvisioning()`
(#6) pull seams, never the `EventBus`. The page is rendered by pure `buildDashboardHead` +
`appendCrackedRow` + `kDashboardFoot` and **streamed** in chunks (`sendContent`), so the up-to-256-entry
account is never held whole in RAM (ADR-0039 decision 4). The operator sets the hardened AP passphrase
in the existing captive-portal setup form (a new `maintpass` field, accepted-but-never-echoed like the
wpa-sec key, ADR-0037 §4 #20). A board with a screen shows a **Maintenance status panel** — SSID, URL,
and count, but no PSKs (the shoulder-surfable panel stays connection-info-only; ADR-0039 decision 11).
Exit is a reboot into Station: a physical power-cycle without BOOT, or a **30-minute no-activity backstop**
that protects the endless-hunt identity. The in-dashboard controls (resume, force-sync) are slice-0041.
No live hunt runs in this phase, so the radio comes up clean as an AP — no live radio reconfiguration
(ADR-0039 decision 1).

## Definition of Done

**Scenario A — `decideBootPhase()` selects Maintenance by precedence (host).**
- **Given** a provisioned device (a usable stored triad) with the maintenance signal asserted
- **When** `decideBootPhase()` runs
- **Then** it returns `Maintenance`; and with the signal *de*-asserted and `staFailCount <
  kStaRetryBudget` it returns `Station`; and **unprovisioned** with the signal asserted it
  returns `Provisioning` (nothing to maintain); and provisioned with the signal de-asserted and
  `staFailCount >= kStaRetryBudget` it returns `Provisioning` (the ADR-0006 #3 STA-fail
  fallback is unchanged).
- **Proof:** `npm run test:native` → `test/test_provisioning` (boot-gate cases).

**Scenario B — the maintenance passphrase validates fail-loud (host).**
- **Given** a candidate maintenance passphrase
- **When** it is validated
- **Then** empty passes (the AP falls back to `kSoftApPassword`), a 1–7-character value **fails**
  (below the WPA2-PSK minimum `WiFi.softAP()` would reject — a loud rejection, never a silent
  drop to an open AP), and a value of 8+ characters up to the bound passes.
- **Proof:** `npm run test:native` → `test/test_provisioning` (validation).

**Scenario C — `maintenancePass` round-trips with a behaviour-preserving absent default (host).**
- **Given** a record persisted with a `maintenancePass`, and separately a record written by a
  build that predates the field
- **When** each is loaded
- **Then** the first reads back the stored passphrase, and the pre-existing one reads back an
  **empty** `maintenancePass` (fallback to `kSoftApPassword`) — no behaviour change for a device
  provisioned before this slice, exactly like the ADR-0035 selector defaults.
- **Proof:** `npm run test:native` → `test/test_provisioning_store`.

**Scenario D — the streamed dashboard renders persisted results, PSKs included (host).**
- **Given** a `DashboardStats` (recovered count, capture-queue depth, `deauthArmed`, `everSynced`) and
  a `CrackedResult` carrying `{essid, psk}`
- **When** `buildDashboardHead()` renders the summary and `appendCrackedRow()` renders the result
- **Then** the head contains the counts, the arm state, and the ever-synced line (from the persisted
  fresh flag — **not** a RAM last-sync status, ADR-0039 decision 4), and the row contains the network's
  ESSID **and its plaintext PSK** (ADR-0039 decision 5); a crafted ESSID/PSK is HTML-escaped so it
  cannot inject markup.
- **Proof:** `npm run test:native` → `test/test_dashboard`; `test/test_cracked_manifest` for the
  `entryAt()` enumeration seam the serve path streams over.

**Scenario E — each rendered piece fails loud rather than truncating (host).**
- **Given** an output buffer too small for the head, or for a row
- **When** `buildDashboardHead()` / `appendCrackedRow()` is called
- **Then** it returns 0 and writes no partial output (the caller serves 500 for a bad head, and a row
  that cannot fit its own bounded buffer is a bug surfaced loud, never a truncated fragment in the
  stream), mirroring `buildSetupForm()` (ADR-0037).
- **Proof:** `npm run test:native` → `test/test_dashboard`.

**Scenario H — the setup form accepts the maintenance passphrase without echoing it (host).**
- **Given** a `SetupFormModel` with `hasStoredMaintenancePass=true`, and a re-save that leaves the
  `maintpass` field blank
- **When** `buildSetupForm()` renders and `resolveProvisioningUpdate()` merges
- **Then** the form shows a "leave blank to keep" hint and **no** `value=` for the passphrase (§4 #20,
  no secret echoed), and the merge keeps the stored passphrase on a blank re-save (replaces it on a
  non-blank value) — exactly like the wpa-sec key.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario F — the no-activity backstop becomes due at the bound (host).**
- **Given** a maintenance entry time and a last-activity time
- **When** `kMaintenanceBackstopMs` (30 min) elapses since the last activity
- **Then** the backstop reports **resume-due**; before the bound it does not; and recorded
  activity pushes the due time forward — so an in-use dashboard never self-reboots mid-review,
  but a walked-away unit resumes hunting (ADR-0039 decision 7).
- **Proof:** `npm run test:native` → `test/test_dashboard` (backstop arithmetic).

**Scenario G — the device enters Maintenance on BOOT-hold, serves over the hardened AP, shows the
panel, and resumes (device, review-only + verify script).**
- **Given** a provisioned device with a `maintenancePass` set (bench: `cardputer_testhooks` built with
  `SAPPER_TEST_MAINT=1` + `SAPPER_TEST_MAINT_PASS`, since the physical BOOT-hold cannot be driven
  headlessly — a build-time hook, never a serial path, §4 #7)
- **When** it is powered on with BOOT held (or the hook forces it)
- **Then** it reports `[STATE] phase=maintenance`, raises SoftAP `Sapper-XXXX` requiring the
  `maintenancePass` (not `sapper-setup` and not open), serves the dashboard listing the persisted
  recovered results (incl. a seeded plaintext PSK), draws the Maintenance status panel (SSID + URL +
  count, no PSKs) on a screened board, keeps answering serial while parked (§4 #3), and — on a
  power-cycle without BOOT, or after the 30-minute backstop — reboots into Station and resumes hunting.
- **Proof:** `npm run verify:maintenance` → `scripts/0040-maintenance-verify.mjs` (phase entry + the
  `[MAINT]` banner + serial liveness machine-checkable over serial; the AP-passphrase + served-dashboard
  half via an HTTP fetch once joined to the AP). The panel draw and the resume power-cycle are eyeball /
  physical (the boot gate's Station return without the button is host-tested, Scenario A); the SoftAP
  serve path is the one already proven by `npm run verify:provisioning`.

## Verification

- **Host (`npm run test:native`)** — Scenarios A–F, H: `test/test_provisioning` (boot-gate
  precedence incl. Maintenance + passphrase validation + the `maintenance` phase label),
  `test/test_provisioning_store` (`maintenancePass` round-trip + absent-default), `test/test_dashboard`
  (streamed head/row render incl. PSK, HTML-escaping, per-piece overflow fail-loud, backstop arithmetic
  incl. millis-wrap safety), `test/test_cracked_manifest` (`entryAt` enumeration + bounds), and
  `test/test_provisioning_form` (the `maintpass` field: keep-on-blank, no `value=` echo).
- **Device (`npm run verify:maintenance`, wired in `package.json` per R11)** — Scenario G:
  `scripts/0040-maintenance-verify.mjs` asserts `phase=maintenance`, captures the `[MAINT]` banner,
  confirms serial stays answered while parked, and (once the workstation is joined to the AP) fetches
  the dashboard and confirms the Maintenance page + a seeded PSK render. This behaviour (BOOT-hold
  entry, live SoftAP passphrase, streamed serve, mode lifecycle) is not host-assertable, so it ships a
  verify script (§3), unlike slice-0038.
- The BOOT GPIO read is a thin device seam (review-only, like the other device seams), and the
  `MaintenancePortal` serve/stream + Maintenance panel draw are device-only (review-only); the pure
  boot-gate/validation/render/backstop logic behind them carries the behavioural proof.

## As built

Shipped as designed (ADR-0039), with two design refinements recorded in the ADR during implementation:
the dashboard is **streamed** (`buildDashboardHead` + `appendCrackedRow` + `kDashboardFoot` via
`sendContent`) rather than a single fixed buffer — the page is data-dependent (up to 256 results, ~36 KB)
so one buffer would either overflow the stack or break the viewer on a large account (ADR-0039 decision 4,
a §2 correction to the draft); and the dashboard shows **ever-synced** from the persisted manifest fresh
flag, not a RAM last-sync status the reboot would wipe.

- **New pure module `net/dashboard.{h,cpp}`:** `DashboardStats`, `buildDashboardHead`, `appendCrackedRow`
  (HTML-escaping ESSID/PSK), `kDashboardFoot`, and `maintenanceBackstopDue` (millis-wrap-safe, 30-min
  bound). `net/provisioning.{h,cpp}` gained `Phase::Maintenance` + its `maintenance` label,
  `decideBootPhase`'s repurposed button precedence, and `isUsableMaintenancePass`. `ProvisioningRecord`
  gained `maintenancePass` (NVS key `maint_pass`, absent-default empty). The setup form
  (`net/provisioning_form`, ADR-0037) gained the accepted-but-never-echoed `maintpass` field.
  `CrackedManifest` gained the read-only `entryAt(i)` enumeration view (§4 #12 intact).
- **New device-only `net/maintenance_portal.{h,cpp}`:** the hardened SoftAP that streams the dashboard,
  chosen passphrase = `maintenancePass` or the `kSoftApPassword` fallback. `main.cpp` reads BOOT/GPIO0
  (`maintenanceRequested()`, board-profile `bootButtonPin`), dispatches `runMaintenance()` (loads the
  manifest + capture-queue depth, draws the Maintenance panel, serves, and reboots on the no-activity
  backstop). `platformio.ini` gained `SAPPER_TEST_MAINT`/`_MAINT_PASS` (headless entry hook + passphrase
  seed).
- **Host:** all 27 native suites pass (0 failures, warning-clean under `-Wall -Wextra`) — `test_dashboard`
  9 (Scenarios D/E/F incl. escaping + wrap-safety), `test_provisioning` 26 (Scenarios A/B + label),
  `test_provisioning_store` 11 (Scenario C), `test_provisioning_form` 15 (Scenario H),
  `test_cracked_manifest` 9 (`entryAt`). Scenario A's fail-before is inherent: the old
  `decideBootPhase(true,0,true)` returned `Provisioning`, so the new Maintenance assertion fails against
  the pre-change gate and passes after.
- **Boards:** both build clean under `-Wall -Wextra` — `cardputer` Flash 41.0% (1,369,031 bytes),
  `cardputer_testhooks` 41.0% (1,369,679 bytes), RAM 35.6%.
- **Docs:** `npm run lint` and `npm run index` green; README gained a Maintenance-mode section; LEDGER
  closed the web-dashboard item, corrected the rule-of-three note, and recorded the given-up cases. §4
  invariant #21 added.
- **Device (Scenario G):** `scripts/0040-maintenance-verify.mjs` wired as `verify:maintenance` (R11);
  its serial half (phase + `[MAINT]` banner + liveness) and optional HTTP half (served dashboard + seeded
  PSK) are the machine-checkable proof, the panel draw and resume power-cycle the eyeball/physical half.
  Not run in this environment (no board attached); it is the standing regression check.
- **Adversarial pass (Sonnet 5, blind):** no correctness finding meeting the bar (crash/overflow/wrong
  output/wrap). The reviewer hand-traced the escape-buffer arithmetic (worst case 193/385 B ≤ the 198/390 B
  buffers; a row ~618 B ≤ 1024), confirmed `decideBootPhase` precedence + every caller, the no-echo
  guarantee, `maintenanceBackstopDue` wrap-safety, and that §4 #6/#12/#13/#20/#21 hold (#13 correctly
  *revised* by ADR-0039 decision 3, not violated; the PSK-display exception recorded, not silent). It
  could not run the native lane (the known-broken pio `native` platform here), so its findings are
  static. Three accuracy/robustness points it raised were fixed: (1) the ADR-0039 decision-2 /
  `decideBootPhase` doc prose overstated the config-form-in-Maintenance capability as present-tense (it
  is slice-0041) — reworded to future/STA-fall-back-today; (2) `BoardProfile::bootButtonPin` gained a
  `= -1` default member initializer so a future board that omits it defaults to "absent" rather than
  GPIO0 (which would spuriously enter Maintenance) — the one mode-gating pin; (3) a cosmetic
  entity-length comment in `dashboard.h`. Post-fix: lint green, `test_board_profile` green, `cardputer`
  rebuilds clean.
