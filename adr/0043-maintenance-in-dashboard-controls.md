---
id: '0043'
title: "In-dashboard Maintenance controls — reboot-scoped resume and re-provision"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

slice-0040 shipped the read-only half of Maintenance mode (ADR-0039): a BOOT-hold enters a
hunt-suspended boot phase that raises a hardened SoftAP and serves the persisted recovered
results. ADR-0039 decision 8 deferred the *write* half — the in-dashboard controls — to this
slice, and named three of them, all as **reboot-scoped intents** (a surface never owns a live
engine; it schedules something the next boot honours, extending ADR-0021 #13):

1. **resume** — restart into Station;
2. **force sync now** — a persisted flag consumed on the next Station boot;
3. **re-arm deauth / re-provision** — the existing config form, served inside Maintenance.

Building it surfaces one correction to decision 8's own framing (recorded per global §2 —
evidence over a prior decision, even one of ours):

**Control 2 (force-sync-next-boot) is behaviourally identical to control 1 (resume).** The
sync scheduler is due-at-boot by design: `SyncScheduler::begin(nowMs)` sets the next deadline
to *now* (`sync_scheduler.h`), and every Station boot calls `g_scheduler.begin(millis())`
(`hunt_loop.cpp`), so `isDue()` is true on the first window. Per ADR-0019 decision 6 the
supervisor then forces a sync-only STA window at boot even when the upload queue is empty. The
scheduler is RAM-only, wiped by the reboot that entered Maintenance, so this holds regardless
of when the last sync ran. Therefore **rebooting into Station already forces a sync on the
first window** — "force sync now" and "resume" reboot into the same path and produce the same
sync. A separate control would be a relabelled resume button: indirection with no distinct
behaviour to encode (§3 KISS). Decision 8 anticipated three controls; two suffice.

Two further forces shape the controls:

- **The Maintenance server accepts inbound HTTP and answers *any* path with a page** (its
  `onNotFound` serves the dashboard so a browser lands on it directly — captive-portal shape,
  ADR-0039). OS captive-check probes and browser prefetch fire **GET** requests at arbitrary
  paths. A control that mutates state or reboots on GET would therefore fire on a prefetch or
  a background connectivity check, not only on an operator click.
- **Re-provisioning already has a pure, no-echo form seam** (`net/provisioning_form`,
  ADR-0037/§4 #20): `buildSetupForm` renders from a secret-free model and
  `resolveProvisioningUpdate` merges a submission over the stored record without wiping an
  un-retyped secret. The captive portal (Provisioning phase) already wires these; Maintenance
  needs the same two decisions, differing only in *where* the form is reached.

## Decision

**Two controls, served on the Maintenance SoftAP's existing WebServer — both reboot-scoped,
both POST-gated.**

1. **Resume & hunt.** `POST /resume` sets a pending-reboot flag; the `runMaintenance` loop
   performs `ESP.restart()` after the HTTP response flushes (the handler never restarts
   mid-response). A clean power-on without BOOT re-enters Station via the ADR-0039 boot gate,
   and — per the Context finding — the first STA window syncs. This is the whole of "resume"
   and, implicitly, of "sync now".

2. **Re-provision / re-arm.** `GET /config` renders `buildSetupForm()` from a `SetupFormModel`
   built off the loaded record (secret-free by construction, §4 #20); the form posts to
   `POST /save` (the action the pure form already carries, so it is unchanged), which parses
   the submission, merges it with `resolveProvisioningUpdate()` over the stored record,
   persists via `persistProvisioning()` (§4 #6 — the single NVS write seam), and sets the
   pending-reboot flag on success. This re-arms deauth, flips the notify selectors, changes
   WiFi/key/webhook, and sets the Maintenance passphrase — the full config surface — on a
   *reachable* device, which until now was reconfigurable only through the STA-fail fallback
   portal (closes the `LEDGER.md` on-demand re-provision item).

3. **All mutating actions are POST; only reads are GET.** `GET /` (dashboard) and `GET /config`
   (form) are safe to prefetch; `POST /resume` and `POST /save` are not reachable by a GET, so
   an OS captive-check or a browser prefetch cannot reboot the device or rewrite provisioning.
   This is a correctness requirement, not a style choice — the server answers arbitrary GET
   paths with a page (ADR-0039), so a GET-triggered mutation *would* fire unbidden.

4. **The controls schedule reboot-scoped intents only — never live engine mutation (extends
   ADR-0021 #13).** There is no engine running in Maintenance; a control cannot and does not
   reach into one. `resume` restarts; `save` persists and restarts. The next Station boot
   honours the new record and the always-due first sync. A surface still never owns the live
   engine — the load-bearing boundary this slice must not cross.

5. **The controls' HTML lives in the pure `net/dashboard` module and is host-tested.** A new
   `kDashboardControls` chunk (the resume POST form + the `/config` link) is streamed after the
   rows and before the footer, keeping the whole page unmaterialised (ADR-0039 decision 4). The
   safety property — resume is a POST, not a GET — is then a host-tested fact
   (`test_dashboard`), so a regression to a GET link fails the native lane. The device wiring
   (route registration, `ESP.restart`, arg parsing) stays device-only and review-only /
   verify:device, like every other served-surface wiring.

6. **The form-save device wiring is duplicated, deliberately, per the rule of three.** Parsing
   the submitted args into a `ProvisioningRecord` and running resolve→persist appears in the
   captive portal (Provisioning) and now in the Maintenance portal — the **second** instance.
   The *pure* policy (`buildSetupForm`/`resolveProvisioningUpdate`) is already shared; only the
   `WebServer::arg` plumbing repeats. Per §3 the rule of three does not fire on the second
   instance; the duplication is recorded in `LEDGER.md`, to be hoisted into a shared
   `readSubmittedRecord(WebServer&, …)` helper only when a third serving surface appears.

7. **Device behaviour a unit test cannot assert extends the existing verify script (§3).**
   `scripts/0040-maintenance-verify.mjs` (`verify:maintenance`, already wired, R11) gains the
   control checks: `POST /resume` reboots into Station; `GET /config` serves the setup form;
   `POST /save` persists and reboots. No new verify script — this is the same served surface
   the slice-0040 script already drives, so a dedicated one would copy its SoftAP/HTTP
   machinery for no new question (§3, the rule of three).

## Consequences

- **Adds §4 standing invariant #23** in the slice-0044 commit: the Maintenance controls mutate
  only via POST-gated, reboot-scoped intents — never live engine ownership — and re-provision
  writes only through the §4 #6 seam with the §4 #20 no-echo form.
- **Closes the `LEDGER.md` on-demand re-provision item.** A reachable, provisioned device can
  now be fully reconfigured (WiFi, key, webhook, deauth arm, notify selectors, Maintenance
  passphrase) without the STA-fail fallback — reached by a physical BOOT-hold, which is the
  correct gate for a control surface that also exposes recovered PSKs (ADR-0039 decision 5).
- **Drops force-sync-next-boot as redundant, recorded here rather than silently.** Decision 8's
  third control collapses into resume because the scheduler is already due at every boot; a
  `LEDGER.md` note records that an explicit "sync now" affordance would be a duplicate button,
  to be revisited only if the scheduler ever stops being due-at-boot.
- **Extends ADR-0021 #13** (observers never own the engine — controls are reboot-scoped
  intents) and **ADR-0039** (the write half of the Maintenance phase) and **ADR-0037** (reuses
  the no-echo form seam). Supersedes none: ADR-0039 decision 8's prose stays true as the
  intent recorded on its date; this ADR records what building it found.
- **The MaintenancePortal constructor takes the full `ProvisioningRecord`** (not just the two
  fields slice-0040 needed), because the config form must render the record's `hasStored*`
  facts and merge a re-save over the stored record. A device-only interface change, review-only.
