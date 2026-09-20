---
id: '0037'
title: "Stateful setup form rendered from a secret-free model; a blank secret field keeps the stored value"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

The captive-portal setup form (`kFormHtml` in `captive_portal.cpp`, ADR-0006) is served *static*: it
carries no device state, so every re-open shows the same value-free defaults. That was harmless when the
form held only the association triad — but the form has since grown toggles that carry real state:
`deauthEnabled` (ADR-0029), the `webhookUrl` (ADR-0023), and, most recently, the three-way push selector
(`notifyCaptured`/`notifyCracked`/`notifySyncError`, ADR-0035, §4 #19). A static form silently resets
all of them on any re-save:

- The portal is re-entered not only on first boot but on the **STA-fail fallback** (`decideBootPhase`,
  ADR-0006 #3): a device that cannot join its configured network re-opens for reconfiguration. That is
  precisely when the stored record is *valid and populated* — the operator fell in to fix the WiFi
  credentials, and on saving the form re-applies the value-free defaults: deauth disarms, the notify
  selector resets to "cracked only", and a blank webhook field disables push. All three are fail-safe
  (nothing dangerous happens) but **silent** — the operator changed one field and unknowingly reverted
  three others, because the form never showed their stored state.
- slice-0035 widened this: it added two more silently-resettable checkboxes to the same static form, and
  its own ADR flagged the interaction as a LEDGER debt rather than fixing it there.

The fix is a *stateful* form that renders the stored state back. The reason it was deferred is a real
hazard: the `webhookUrl` is a **secret** — an ntfy topic or a Discord webhook token is a bearer
credential — and the wpa-sec `key` and WPA `pass` are secrets too. Naively echoing a stored `webhookUrl`
into the served HTML would hand that credential to anyone who joins the (briefly open, fixed-password)
setup AP and opens the page. So a stateful form must render the *toggles* back without ever echoing a
*secret* back.

A design fork the operator settled before this ADR: **how does a re-saved blank secret field behave?**
The choice was **blank keeps the stored value** (type to replace), not "blank clears". The reasoning: the
dominant re-provision trigger is an STA failure, where the operator re-types the WiFi SSID + passphrase
(the pair they came to fix) and touches nothing else — so the wpa-sec key and webhook URL they did *not*
retype must survive. Disabling push is still reachable without echoing or clearing the URL: unchecking the
three notify boxes turns push off regardless of the stored URL. The one behaviour this gives up —
switching an already-provisioned webhook back to "no URL", or a WPA network back to open *while keeping the
same SSID* — is a rare case on a fixed-site appliance and is recorded as a LEDGER limitation (there is no
factory-reset path either; that is a separate deferred item).

## Decision

1. **A pure, secret-free `SetupFormModel` drives rendering; a pure `buildSetupForm()` produces the HTML.**
   A new pure module `net/provisioning_form.{h,cpp}` (host-tested, ADR-0004 lane 1) holds
   `SetupFormModel { bool deauthArmed; bool notifyCaptured; bool notifyCracked; bool notifySyncError;
   bool hasStoredKey; bool hasStoredWebhook; }` and `size_t buildSetupForm(const SetupFormModel&, char*
   out, size_t outSize)`. The model carries **only booleans** — no `ssid`, `pass`, `key`, or `webhookUrl`
   string. A secret therefore *cannot* reach the served HTML: the render function is structurally unable
   to interpolate one, because it is never handed one. This mirrors the frame-free `CaptureFact` of #17
   and the password-field-free `WebhookNotification` of #19 — the guarantee holds by construction, not by
   review vigilance. `buildSetupForm` returns 0 (writing nothing) if the form would not fit `outSize` —
   fail loud, never a truncated page (§3).

2. **The form renders the four toggle states and two "a secret is stored" hints.** Each checkbox
   (`deauth`, `nCaptured`, `nCracked`, `nSyncErr`) is emitted `checked` iff its model bool is set, so an
   unchecked box on a re-save is now a *deliberate* disarm the operator can see, not a silent reset. When
   `hasStoredKey` the wpa-sec key `<input>` drops its `required` attribute and shows a "leave blank to
   keep" placeholder; likewise the webhook field when `hasStoredWebhook`. The secret values themselves are
   never rendered.

3. **A blank secret field keeps the stored value; a pure `resolveProvisioningUpdate()` decides.** Also in
   `provisioning_form.{h,cpp}`: given the submitted record, the stored record, and whether one was stored,
   it produces the record to persist — `key` and `webhookUrl` blank in the submission are taken from the
   stored record (kept), non-blank replace it; `ssid` and `pass` are always taken from the submission
   (the pair the operator re-enters; a blank `pass` stays an open network, ADR-0006, so switching to open
   *is* reachable by retyping the SSID with a blank passphrase); the four booleans are always taken from
   the submission (the form now shows their state, so the checkbox is authoritative). On first boot
   (nothing stored) every field is taken from the submission verbatim, so a blank key still fails
   `validateCredentials` loudly as before — "keep" only applies over an existing value.

4. **The seam is unchanged (#6): `loadProvisioning()` in `handleRoot` and `handleSave`.** `handleRoot`
   loads the stored record (or defaults on none — `notifyCracked` true, matching the ADR-0035 read-default
   and portal default), maps it to a `SetupFormModel`, and serves `buildSetupForm()`'s output.
   `handleSave` loads the stored record, merges via `resolveProvisioningUpdate()`, then validates and
   persists the *merged* record exactly as before. No new NVS access site — the §4 #6 single-seam
   invariant holds.

5. **The novel logic is host-tested; no new device verify script.** Rendering (checkbox state, the
   `required`-drop, buffer-overflow fail-loud) and the merge (keep-on-blank, replace-on-value,
   always-from-submission for the pair and the toggles, first-boot verbatim) are pure and proven in
   `test/test_provisioning_form`. The device wiring (portal serves the built form and persists the merged
   record) is the same serve/persist path the slice-0007 provisioning verify already exercises on
   hardware; adding a second hardware verify for a purely-host-provable behaviour change would be a
   one-shot dead script (§3's own caution), so this slice ships none. The portal→NVS wiring stays
   review-only, like the other portal wiring rows.

## Consequences

- `captive_portal.cpp` loses its static `kFormHtml`; `handleRoot` builds the form from a `SetupFormModel`
  and `handleSave` merges through `resolveProvisioningUpdate()`. A new pure `net/provisioning_form.{h,cpp}`
  is added to the native lane (`platformio.ini build_src_filter`) and the board build (default glob).
- New §4 invariant **#20** records that the setup form is rendered by `buildSetupForm()` from a
  secret-free `SetupFormModel`, so no provisioned secret (`pass`/`key`/`webhookUrl`) is ever interpolated
  into served HTML, and that a re-save keeps a stored secret whose field is left blank. Added in this
  ADR's commit (same-commit rule).
- Re-save behaviour changes for operators: a stored wpa-sec key or webhook URL now survives a re-save that
  leaves its field blank (previously the key was re-required and a blank webhook disabled push). Push is
  disabled by unchecking the notify boxes, not by clearing the URL. README updated in this commit.
- **Not reachable via the portal (recorded, not fixed):** switching an already-provisioned webhook back to
  "no URL configured", or a WPA network to open *without changing the SSID*. Both are rare on a fixed-site
  appliance, and there is no factory-reset path either (a separate LEDGER item). A LEDGER line records it.
- ADR-0006 is extended, not superseded: the seam, the validation, and the SoftAP/DNS shape are unchanged;
  the form simply renders and merges stored state instead of serving a static page. ADR-0029/-0023/-0035,
  whose toggles the static form was silently resetting, are unchanged — this closes the interaction each
  of them deferred to the LEDGER.
