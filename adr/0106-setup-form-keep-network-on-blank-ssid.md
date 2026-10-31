---
id: '0106'
title: "Setup form Wi-Fi pair: keep-on-blank-SSID with an explicit open-network checkbox"
type: architecture
status: accepted
date: 2026-10-31
supersedes: []
superseded_by: []
---

## Context

ADR-0037 made the captive-portal setup form *stateful without leaking a secret*: a secret-free
`SetupFormModel` drives the render, the stored toggles come back checked, and the `key`/`webhookUrl`/
`maintenancePass` fields keep their stored value when submitted blank (type to replace). Its decision 3
deliberately **excluded the Wi-Fi pair** from keep-on-blank: `ssid` and `pass` were always taken from the
submission, and a blank `pass` meant "open network". The reasoning was that the dominant re-provision
trigger is an STA failure, where the operator re-types the exact SSID + passphrase they fell in to fix, so
there was nothing to keep.

Two things that reasoning did not weigh have since made it the wrong default:

- **The form is now served on a *working* device.** slice-0044 added the Maintenance `/config` route
  (ADR-0043), which re-serves this same form. In Maintenance the Wi-Fi is fine: the operator is there to
  flip a toggle, rotate the wpa-sec key or webhook, set the Maintenance passphrase, or change the dim
  delays. Forcing a full SSID + passphrase re-entry to touch any of those is exactly the re-save friction
  ADR-0037 removed for every *other* field. This "light-touch edit" is the common on-site trigger whose
  arrival LEDGER deferred the fix on ("deferred until on-site use shows the light-touch edit is common").

- **"Blank pass = open" is a silent brick footgun on that path.** The same form teaches "leave blank to
  keep" for the three secret fields, so an operator naturally retypes the SSID and leaves the passphrase
  blank, intending to keep it. `validateCredentials` accepts an empty passphrase (an open network), so the
  record is silently persisted as **open**; the unit reboots unable to associate, exhausts the
  `kStaRetryBudget` STA attempts, falls back to the setup portal, and **stops hunting until a physical
  revisit**. The slice-0044 adversarial pass flagged this; the README had to carry an explicit warning
  because the form could not be trusted to teach keep-on-blank for Wi-Fi like it does for the secrets.

The fix LEDGER sketched ("prefill the SSID and add keep-on-blank for the passphrase") is unavailable as
written: prefilling the SSID conflicts with ADR-0037's secret-free model. The first-boot/STA-fail AP uses
the fixed public `kSoftApPassword`, so echoing the configured home SSID into the served HTML would hand it
to anyone in range. The same `buildSetupForm` serves both that public AP and the password-gated
Maintenance AP, so the SSID value must not be rendered on either.

## Decision

1. **The SSID becomes keep-on-blank; the passphrase is then set against the kept name.** `SetupFormModel`
   gains a single boolean `hasStoredNetwork`. When set, the SSID `<input>` drops `required` and shows a
   "leave blank to keep current Wi-Fi" placeholder (no value: the SSID string is still never rendered, so
   §4 #20 holds by construction, exactly as for the secret fields). In `resolveProvisioningUpdate`, a
   **blank submitted SSID on a provisioned device keeps the stored network *name***, and the passphrase is
   resolved against that kept name: a blank passphrase keeps the stored one (the full light-touch edit),
   a **typed** passphrase rotates it (change the Wi-Fi password without re-typing the SSID), and Open makes
   the current network open. The old passphrase is never kept over a typed one — doing so would reboot with
   a stale password and strand the unit exactly as the blank-pass footgun does, reached by a different
   path. **Atomicity is preserved where it matters:** when the SSID is *changed* (a non-blank submission,
   or first boot) the passphrase comes wholly from the submission, so a stored passphrase is never bound to
   a different SSID than the one it was stored with. So the light-touch edit (toggle, secret, or a password
   rotation) needs no network re-entry, while a network *switch* always carries its own passphrase.

2. **Open networks become explicit, not blank-pass.** A new `open` checkbox ("Open network (no
   passphrase)") is always rendered. Entering a **new** network (a non-blank submitted SSID, or first
   boot) with a blank passphrase and the box **unticked** is refused loudly by a new pure
   `validateNetworkEntry()` (`NetworkEntryError::PassphraseRequired`) at both save sites. Blank no longer
   silently means open, so the brick footgun is closed on the Maintenance path and on first boot alike.
   When the box **is** ticked, `resolveProvisioningUpdate` forces the passphrase empty (the checkbox is
   authoritative over any stray text), so an open network is stored unambiguously — whether a new SSID was
   entered or the current one is kept (SSID left blank), the latter switching the *current* network to open
   without re-typing its name, which closes the ADR-0037 "Wi-Fi to open while keeping the same SSID"
   limitation.

3. **Secret-free guarantee preserved; one shared form.** The model gains only a boolean. No `ssid`/`pass`
   string ever reaches the render, so the no-echo guarantee holds by construction on both the public setup
   AP and the gated Maintenance AP, and the single `buildSetupForm` keeps serving both. This extends
   ADR-0037's model; it does not weaken it.

4. **Host-tested; no new device verify.** The new render branches (SSID `required`-drop + keep
   placeholder, the Open checkbox, the Wi-Fi-help line), the merge (keep-both-on-blank-SSID, open forces
   empty pass), and `validateNetworkEntry` are pure and proven in `test/test_provisioning_form`. The device
   wiring (both portals parse `open`, run the guard, persist the merged record) is the same serve/persist
   path the slice-0007 provisioning verify and the slice-0040 Maintenance verify already exercise on
   hardware, so no one-shot device script is added (ADR-0037 decision 5's reasoning, unchanged).

**Why ADR-0037's decision 3 was narrow, not wrong at the time.** When it shipped (slice-0038), the form was
served only on first boot and the STA-fail fallback, where re-typing the pair *was* the whole task, so
excluding the pair from keep-on-blank cost nothing and blank-pass-is-open was a tidy way to reach an open
network. slice-0044's `/config` route then made "edit one field on an associated device" a first-class
trigger, and against that workflow the always-re-enter rule became the very silent-reset friction ADR-0037
set out to kill, while blank-pass-is-open turned into an active brick hazard. This ADR therefore **extends
ADR-0037, and supersedes neither it nor ADR-0006**: the secret-free model, the key/webhook/maintpass
keep-on-blank, the buffer fail-loud, the seam, and the SoftAP/DNS shape are all unchanged.

## Consequences

- `SetupFormModel` gains `hasStoredNetwork`. `resolveProvisioningUpdate` gains a `bool openNetwork`
  parameter. A new pure `validateNetworkEntry()` / `NetworkEntryError` lands in
  `net/provisioning_form.{h,cpp}`. `buildSetupForm` renders the conditional SSID/passphrase fields, the
  Open checkbox, and a Wi-Fi-help line. `test/test_provisioning_form` gains cases for each.
- Both save sites (`CaptivePortal::handleSave`, `MaintenancePortal::handleSave`) parse the `open` checkbox,
  pass it to `resolveProvisioningUpdate`, and run `validateNetworkEntry` before persisting. The duplicated
  `WebServer::arg` plumbing stays duplicated (rule of three, ADR-0043 decision 6); only the pure policy is
  shared.
- §4 invariant **#20** is refined in this commit (same-commit rule): the Wi-Fi pair is now keep-on-blank-
  SSID, and a new network needs a passphrase unless Open is ticked. The no-secret-echo half is unchanged.
- Operator-facing behaviour: leave the SSID blank to keep the current Wi-Fi (passphrase included); to
  switch networks, type the new SSID and its passphrase, or tick Open for a passphrase-less network. A new
  network with a blank passphrase and Open unticked is now a loud 400, not a silent open-network brick.
  README updated in this commit.
- Closes the LEDGER light-touch-edit footgun item (the Maintenance `/config` silent-open brick) and the
  "Wi-Fi to open while keeping the same SSID" clause of the ADR-0037 limitation item (now reachable by
  leaving the SSID blank with Open ticked). Also supports rotating the passphrase without re-typing the
  SSID (leave the name blank, type the new passphrase) — a case the first cut of this ADR regressed into a
  silent stale-password keep, caught in review before merge. The webhook-back-to-none and factory-reset
  limitations remain.
