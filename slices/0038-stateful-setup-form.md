---
id: '0038'
title: "Stateful setup form: render stored toggles back without echoing secrets; a blank secret field keeps its stored value"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Close the LEDGER portal-statelessness defect (ADR-0029/-0023/-0035 all deferred it): the captive-portal
setup form is served static, so re-opening it — the STA-fail fallback, ADR-0006 #3 — and saving silently
resets the deauth arm, the three-way push selector, and (by blanking) the webhook. Per ADR-0037: extract a
pure `net/provisioning_form.{h,cpp}` holding a secret-free `SetupFormModel` + `buildSetupForm()` (renders
the four checkbox states and "leave blank to keep" hints, never a secret) and `resolveProvisioningUpdate()`
(blank `key`/`webhookUrl` keep the stored value; `ssid`/`pass` and the toggles always come from the
submission; first boot verbatim). Wire `handleRoot`/`handleSave` to them through the existing
`loadProvisioning()` seam (#6). No secret ever reaches served HTML — by construction, the render model has
no secret field.

## Definition of Done

**Scenario A — a stored toggle renders back (host).**
- **Given** a `SetupFormModel` with `deauthArmed=true`, `notifyCaptured=true`, `notifyCracked=false`,
  `notifySyncError=true`
- **When** `buildSetupForm()` renders it
- **Then** the `deauth`, `nCaptured`, and `nSyncErr` inputs are emitted with `checked` and `nCracked` is
  not — so an unchecked box on re-save is a visible, deliberate choice, not a silent reset.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario B — a secret is never echoed, and its field invites "keep" (host).**
- **Given** a model with `hasStoredKey=true`, `hasStoredWebhook=true`
- **When** the form is rendered
- **Then** the wpa-sec key input drops `required` and shows a "leave blank to keep" hint, the webhook
  field shows the same, and (by construction — the model has no secret string) the output contains no
  `pass`/`key`/`webhookUrl` value; with `hasStoredKey=false` the key input keeps `required`.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario C — a blank secret keeps the stored value on re-save (host).**
- **Given** a stored record with a wpa-sec key and a webhook URL, and a submission that re-enters SSID +
  passphrase but leaves key and webhook blank
- **When** `resolveProvisioningUpdate(submitted, stored, hasStored=true, out)` merges them
- **Then** `out.key` and `out.webhookUrl` equal the stored values (kept), while `out.ssid`/`out.pass` are
  the submitted ones — a re-save to fix WiFi does not wipe the secrets the operator did not retype.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`. (Fail-before confirmed by stubbing the
  merge to take the submission verbatim: these keep-assertions fail; they pass with the real merge.)

**Scenario D — a non-blank secret replaces, and toggles/pair always come from the submission (host).**
- **Given** a stored record, and a submission with a new key, a new webhook, both notify boxes flipped
  from their stored state, and a different SSID/passphrase
- **When** the merge runs
- **Then** `out` takes the new key, the new webhook, the submitted toggle states (a deliberate disarm is
  honoured), and the submitted SSID/passphrase — nothing is silently kept when a value was supplied.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario E — first boot keeps nothing; a blank key still fails loud (host).**
- **Given** no stored record (`hasStored=false`) and a submission with a blank key
- **When** the merge runs and the merged record is validated
- **Then** `out.key` is blank (nothing to keep) and `validateCredentials` returns `KeyEmpty` — "keep"
  only applies over an existing value, so first-boot validation is unchanged.
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario F — the form fails loud rather than truncating (host).**
- **Given** an output buffer too small for the rendered form
- **When** `buildSetupForm()` is called
- **Then** it returns 0 and writes no partial page (the caller serves a 500, never a broken half-form).
- **Proof:** `npm run test:native` → `test/test_provisioning_form`.

**Scenario G — the portal serves the built form and persists the merged record (device, review-only).**
- **Given** a provisioned device that fell into the STA-fail portal
- **When** the operator opens the portal, sees the armed/selected toggles checked, re-enters WiFi, leaves
  key + webhook blank, and saves
- **Then** the device persists the WiFi change while keeping the stored key/webhook and toggle state, and
  reboots into STA — the same serve/persist path proven on hardware by the slice-0007 provisioning verify.
- **Proof:** review of the `handleRoot`/`handleSave` wiring (ADR-0037 #4/#5); `npm run verify:provisioning`
  covers the underlying portal-serves-and-persists path.

## Verification

- **Host (`npm run test:native`)** — `test/test_provisioning_form` proves Scenarios A–F: checkbox-state
  rendering, the `required`-drop + keep hints, no-secret-by-construction, buffer-overflow fail-loud, and
  the merge policy (keep-on-blank, replace-on-value, always-from-submission for the pair and toggles,
  first-boot verbatim). Scenario C's fail-before was confirmed against a verbatim-merge stub.
- **Device (review-only)** — Scenario G: the `handleRoot`/`handleSave` wiring through `loadProvisioning()`
  (§4 #6), with the portal serve/persist path already covered by `npm run verify:provisioning`.
- No new verify script: the novel behaviour is fully host-provable, so a second hardware script would be a
  one-shot dead check (§3).

## As built

Shipped as designed (ADR-0037). The pure `net/provisioning_form.{h,cpp}` module holds
`SetupFormModel` (secret-free — booleans + two `hasStored*` flags), `buildSetupForm()`, and
`resolveProvisioningUpdate()`; `captive_portal.cpp` lost its static `kFormHtml` and now builds the form
from the loaded record in `handleRoot` and merges the submission over the stored record in `handleSave`,
both through the ADR-0006 #6 `loadProvisioning()` seam. §4 invariant #20 records the guarantees.

- **Host:** `test/test_provisioning_form` — 12 tests, all pass (Scenarios A–F): checkbox-state render, the
  `required`-drop + keep hints, the no-`value=`-prefill guarantee, buffer-overflow fail-loud, and the
  merge (keep-on-blank, replace-on-value, toggles/pair always-from-submission, first-boot verbatim +
  blank-key-still-fails). Scenario C's fail-before confirmed against a verbatim-merge stub
  (`Expected 'storedkey' Was ''`); the full native suite stays green.
- **Boards:** both build clean under `-Wall -Wextra` (no warnings in our code) — `cardputer` Flash 40.8%
  (1,363,687 bytes), `cardputer_testhooks` 1,364,323 bytes.
- **Docs:** `npm run lint` and `npm run index` green (0/0); README re-provisioning section updated; LEDGER
  statelessness item closed and the webhook-clear limitation recorded.
- **Device (Scenario G):** review-only wiring, with the portal serve/persist path covered by
  `npm run verify:provisioning`; no new verify script (the novel behaviour is fully host-provable, §3).
- **Adversarial pass (Sonnet 5, blind):** no BLOCKER, no findings meeting the bar. The reviewer
  independently compiled and ran the 12 tests and mutation-tested the keep-on-blank and toggle branches,
  confirming the tests bite. It noted one *inherited, non-regressed* edge (a present-but-invalid stored
  triad reads as first-boot under `loadProvisioning`'s usable-or-nothing gate, ADR-0006) — pre-existing,
  out of this slice's scope.
