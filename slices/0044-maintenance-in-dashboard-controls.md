---
id: '0044'
title: "In-dashboard Maintenance controls: resume and re-provision, reboot-scoped"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0043: add the *write* half of Maintenance mode — two reboot-scoped controls on
the existing Maintenance SoftAP dashboard.

- **Resume & hunt** — `POST /resume` reboots into Station (which syncs on its first window).
- **Re-provision / re-arm** — `GET /config` serves the pure `buildSetupForm()`; `POST /save`
  merges the submission over the stored record (`resolveProvisioningUpdate`), persists via
  `persistProvisioning()` (§4 #6), and reboots.

Both are **POST-gated** for mutation (a GET reboot/wipe would fire on an OS captive-check or
browser prefetch, ADR-0043 decision 3). Force-sync-next-boot (ADR-0039 decision 8's third
control) is **dropped** as redundant with resume — the scheduler is already due at every boot.
The controls schedule reboot-scoped intents only; a surface never owns a live engine (extends
ADR-0021 #13). Adds §4 invariant #23; closes the `LEDGER.md` on-demand re-provision item.

## Definition of Done

**Scenario A — resume reboots into Station (device).**
- **Given** a provisioned device in Maintenance (BOOT-held boot), its SoftAP up
- **When** the operator submits `POST /resume`
- **Then** the device serves a short "resuming" acknowledgement, then reboots; on the clean
  power-on (no BOOT) it enters Station and the first STA window runs a sync (the always-due
  scheduler, ADR-0043 Context) — it does **not** stay in Maintenance.
- **Proof:** `npm run verify:maintenance` drives `POST /resume` and asserts the reboot →
  Station banner sequence on serial; no `[FATAL]`.

**Scenario B — the config form is served and a re-save persists + reboots (device).**
- **Given** a provisioned device in Maintenance
- **When** the operator opens `GET /config` and submits `POST /save` with an edited field
  (e.g. the deauth arm toggle flipped)
- **Then** `GET /config` returns the setup form with the *stored* toggle/selector state and no
  secret echoed (§4 #20), and `POST /save` persists the merged record through the #6 seam,
  serves a success acknowledgement, and reboots into Station carrying the new record.
- **Proof:** `npm run verify:maintenance` drives `GET /config` + `POST /save` and asserts the
  form serve, the persist, and the reboot; `test_provisioning_form` (already green) proves the
  no-echo/merge policy off-device.

**Scenario C — mutations are POST-only; a GET cannot reboot or rewrite (host + review).**
- **Given** the rendered dashboard controls and the Maintenance route table
- **When** the control HTML and the server wiring are inspected
- **Then** the resume control is a `method='POST'` form to `/resume` (not a link), `/save` is
  registered `HTTP_POST` only, and `/config` (GET) is a read that mutates nothing — so an OS
  captive-check or a prefetch GET at any path serves a page, never a reboot or a persist.
- **Proof:** `test_dashboard` asserts `kDashboardControls` uses `method='POST'` for resume and
  exposes the `/config` entry (fail-before/pass-after); review of the `MaintenancePortal` route
  registration confirms `/resume` and `/save` are `HTTP_POST` (device-only wiring, review-only).

**Scenario D — force-sync is not a separate control (review).**
- **Given** ADR-0039 decision 8 named three controls
- **When** the Maintenance dashboard is inspected
- **Then** there is no distinct "sync now" control — resume alone reboots into the always-due
  sync — and the reason is recorded in ADR-0043 and `LEDGER.md`.
- **Proof:** review against ADR-0043's Context finding (the scheduler is due at every boot).

## Verification

- **Device (`npm run verify:maintenance`, already wired per R11)** — Scenarios A and B:
  `scripts/0040-maintenance-verify.mjs` gains the control drive (`POST /resume`, `GET /config`,
  `POST /save`) atop the slice-0040 entry/AP/serve checks, failing loud on any `[FATAL]`. No new
  verify script — the same served surface answers the new questions, so a dedicated one would
  copy its SoftAP/HTTP machinery for nothing (§3, the rule of three).
- **Host (`test_dashboard`, native lane)** — Scenario C's safety property: the controls chunk
  renders resume as a POST form and exposes the `/config` entry. `test_provisioning_form`
  (unchanged, already green) covers the no-echo/merge policy the config form reuses.
- **Build + review** — Scenario C's wiring and Scenario D: the `MaintenancePortal` route
  registration (`/resume`, `/save` POST; `/config` GET) and the pending-reboot-after-flush loop
  are device-only wiring (review-only, like every served surface); force-sync's absence is a
  review against ADR-0043.

## As built

Shipped as designed (ADR-0043); force-sync dropped as planned. No design changes during
implementation.

- **Controls (pure, host-tested):** a new `kDashboardControls[]` in `net/dashboard` closes the results
  table, then renders the resume `POST` form and the `GET /config` link; streamed after the rows and
  before `kDashboardFoot` (which now closes only the document). `test_dashboard` gained
  `test_controls_expose_a_post_resume_and_a_config_link` — the resume control is a `method='POST'` form,
  not a link, so a regression to a GET fails the native lane (Scenario C safety property).
- **Device wiring (review-only + verify:device):** `MaintenancePortal` gained `GET /config` (renders
  `buildSetupForm` from the stored record, no secret echoed), `POST /save` (parse → `resolveProvisioningUpdate`
  over the stored record → validate → `persistProvisioning` → request reboot), and `POST /resume` (request
  reboot). Both mutations are `HTTP_POST` only; a GET falls through `onNotFound` to the dashboard (a safe
  read). Reboot is requested via `m_pendingReboot` and performed by the `runMaintenance` loop *after* the
  HTTP response flushes — never inside the handler (§4 #23).
- **Constructor change:** `MaintenancePortal` now takes the full `const ProvisioningRecord&` (bound to
  `setup()`'s `creds`, which outlives the never-returning `runMaintenance`) instead of the two fields
  slice-0040 needed, so the config form can render the stored `hasStored*` facts and merge a re-save.
- **Force-sync dropped:** confirmed against `hunt_loop.cpp` (`g_scheduler.begin(millis())` at every Station
  boot) — resume already syncs on the first window; recorded in ADR-0043 and `LEDGER.md`.
- **Rule of three:** the form-save arg-parse wiring is the second instance (captive portal is the first);
  left duplicated per §3, ledgered.
- **Verify:** `scripts/0040-maintenance-verify.mjs` (`verify:maintenance`, already wired) extended to
  assert the dashboard's POST resume form + `/config` link, fetch `GET /config` (form served, optional
  no-echo check), and — opt-in via `SAPPER_MAINT_DRIVE_RESUME=1`, because it reboots — drive `POST /resume`.
  No new verify script (the rule of three). The on-hardware run is the operator's (lane 3), as for slice-0040.
- **Docs:** README's Maintenance section documents both controls; `CLAUDE.md` §4 gained invariant #23.
- **Host:** all 27 native suites pass, warning-clean under `-Wall -Wextra` (only `dashboard.cpp` is
  host-compiled here; `maintenance_portal.cpp` is device-only).
- **Boards:** both build clean under `-Wall -Wextra` — `cardputer_testhooks` Flash 41.1% (1,372,947 bytes),
  RAM 35.6%; `cardputer` builds clean.
- **Adversarial pass (Sonnet 5, blind to intent, aware of law):** no memory-safety, buffer, POST/GET-split,
  lifetime, no-echo (§4 #20), or reboot-sequencing defect. One operational finding: reusing the first-boot
  triad-retype form for a *light-touch* Maintenance edit lets a blank/mistyped Wi-Fi passphrase silently
  reconfigure a deployed unit as open and take it off-network. The behaviour is inherited, deliberate
  ADR-0006/ADR-0037 design (blank pass = open; switch-to-open by retype), so the clean fix needs a
  superseding ADR — out of scope here. Acted on: the README over-claim ("leave a secret field blank to
  keep it") was corrected to warn that SSID/passphrase are always taken from the form, and the hazard is
  ledgered for a future ADR.
