---
id: '0113'
title: "Clientless-PMKID solicitation arm: the provisioned toggle, the setup-form checkbox, the hunt_loop wiring"
type: slice
status: accepted
date: 2026-11-06
supersedes: []
superseded_by: []
---

## Goal

Give the operator the switch that slice-0112 left reachable only from the host test: the
default-off `pmkidSolicit` provisioning arm that turns on clientless PMKID solicitation in
the shipped hunt (ADR-0110 slice 3, the second of the two decoupled arms, #5d). This is the
last clause of ADR-0110 #47, and it closes the LEDGER umbrella line for the feature. Four
moves, no new seam:

- **Provision the arm.** `ProvisioningRecord` gains a `bool pmkidSolicit`, NVS-backed through
  the single `persistProvisioning`/`loadProvisioning` seam (§4 #6) on its own `pmkid_arm` key,
  default **off** and absent-key-loads-off, exactly as `deauthEnabled` ships (ADR-0029). A
  device provisioned before this field, or one whose operator left the box unchecked, never
  solicits: a new active-TX behaviour is armed only by an explicit operator act (§4 #16, the
  ADR-0110 #5d refinement).
- **Render the checkbox.** `SetupFormModel` gains `pmkidSolicit` and `buildSetupForm` draws a
  `name='pmkid'` checkbox beside the deauth arm, rendered `checked` from stored state so a
  re-save does not silently disarm it (ADR-0037, the §4 #20 no-echo model: a toggle is not a
  secret, so its state renders back). `resolveProvisioningUpdate` already takes every toggle
  from the submission wholesale, so the arm merges with no new branch.
- **Wire both portals.** The captive portal and the Maintenance `/config` form both parse
  `arg("pmkid") == "on"` at save and both set `model.pmkidSolicit` at render, the second
  instance of the arm plumbing left duplicated per the rule of three like the deauth arm
  (§3, the pure `resolveProvisioningUpdate` is the shared policy).
- **Arm the shipped loop.** `hunt_loop` injects the `RawTransmitter` into the engine when
  `deauthEnabled || pmkidSolicit` (the injection condition slice-0112 flagged for widening),
  calls `setSolicitArmed(pmkidSolicit)` beside the existing `setDeauthArmed`, and logs a
  `[SOLICIT]` arm line plus a `[SOLICIT] txOk=/txFail=` heartbeat when armed, the
  machine-checkable half of the on-air verify (mirroring the `[DEAUTH]` heartbeat).

The on-air armed-solicitation TX and the real-PMKID capture are proven by a device verify the
operator runs on attached hardware (§4 #5), like every other device-radio proof. This slice
flips ADR-0110 #47 clause (d) to landed and the on-air half of clauses (b)/(c) from pending.

## Definition of Done

- **Given** a provisioned device whose operator checked "Enable PMKID solicitation"
- **When** the record is persisted to NVS and loaded back at the next boot
- **Then** `pmkidSolicit` round-trips true, while a default record and a device provisioned
  before the field existed (no `pmkid_arm` key in NVS) both load it **false** — solicitation
  is never a silent side effect of provisioning credentials

- **Given** a stored record with the solicit arm set
- **When** the setup form is rendered (first-boot, STA-fail re-provision, or Maintenance
  `/config`)
- **Then** the `pmkid` checkbox renders `checked` and no secret is echoed; an unchecked box
  on save is a deliberate disarm (the merge takes the toggle from the submission)

- **Given** the shipped hunt loop beginning with `pmkidSolicit` set (deauth off)
- **When** `huntLoopBegin` wires the engine
- **Then** it injects the `RawTransmitter` (a present transmitter now means "raw-TX
  available", §4 #16 refined), calls `setSolicitArmed(true)` and `setDeauthArmed(false)`, and
  logs the armed `[SOLICIT]` line; a loop with neither arm set injects no transmitter and the
  passive hunt is byte-for-byte unchanged

- **Given** a shipped, solicit-armed binary hunting with an authorized AP on air
- **When** the device runs its per-target Capturing windows
- **Then** it logs a repeating `[SOLICIT] txOk=<N>` heartbeat with N>0 and no
  `[FATAL]`/`[ERROR]`, proving the shipped loop puts the blind auth/assoc pair on the air;
  a solicited PMKID M1, if the AP answers, is captured and uploaded via the normal
  `[CAPTURE]`/`[UPLOAD]` path (opportunistic, environmental, not the gate)

## Design

**The arm field (ADR-0110 #5d, §4 #6).** `ProvisioningRecord` gains `bool pmkidSolicit` beside
`deauthEnabled`, documented the same way (default off; ships in every binary but transmits
only when armed). `provisioning_store.cpp` adds one NVS key, `pmkid_arm` (9 chars, within the
15-char NVS limit), read with `getBool(..., /*default=*/false)` and written with `putBool`
counted into the existing all-or-nothing `wrote` conjunction, so a partial write clears the
namespace and the next boot falls back to the portal rather than keeping a stale arm beside a
fresh triad (the exact `deauth_arm` shape). It is **not** part of `validateCredentials`: an
optional arm like the webhook/deauth, never gating the association triad.

**What was considered and reused:** the whole arm reuses the `deauthEnabled` mechanism
end-to-end, the NVS putBool/getBool pair, the all-or-nothing clear, the stateless-form
checkbox rendered from stored state, and `resolveProvisioningUpdate`'s take-toggles-from-
submission rule. Nothing new is designed: the arm is the second instance of a proven pattern,
which is why the only pure-logic test additions are a store round-trip and a form render, not
a new module. The deliberately-**not**-reused option was folding `pmkidSolicit` into
`validateCredentials` (rejected: it would gate the WiFi triad on an optional arm, the bug the
webhook and deauth fields already avoid).

**The setup form (ADR-0037, §4 #20).** `SetupFormModel` gains `bool pmkidSolicit`, rendered
through the existing `checkedAttr` helper into a new `name='pmkid'` checkbox placed in the
same paragraph as the deauth arm (the two active-TX arms grouped), with a one-line help note
that it solicits a PMKID from PMK-caching APs without a client and is authorized-use-only and
off by default. The model stays secret-free (a toggle, not a secret), so §4 #20 holds by
construction. `resolveProvisioningUpdate` needs no code change: `out = submitted` already
carries every toggle; only its doc comment moves from "four notify/deauth booleans" to five,
naming the solicit arm.

**Both portals (§3 rule of three).** `captive_portal.cpp` sets `model.pmkidSolicit =
stored.pmkidSolicit` in `handleRoot` and `submitted.pmkidSolicit = (m_http.arg("pmkid") ==
"on")` in `handleSave`; `maintenance_portal.cpp` does the same in `handleConfig`/`handleSave`.
This is the second copy of the arm plumbing, left duplicated exactly as the deauth arm is
(the LEDGER already tracks the captive/Maintenance save-wiring duplication, ADR-0043 #6): the
shared part is the pure `resolveProvisioningUpdate`, the repeated part is the `WebServer::arg`
read, and a premature abstraction over two call sites is the over-DRY §3 forbids.

**The shipped loop (ADR-0110 #5d, the §4 #16 refinement).** `hunt_loop` gains a file-scope
`g_solicitArmed`, set from `g_creds.pmkidSolicit`. The engine's transmitter injection widens
from `g_deauthArmed ? &g_rawTx : nullptr` to `(g_deauthArmed || g_solicitArmed) ? &g_rawTx :
nullptr`, so the raw-TX path is present whenever *either* arm is set, and the engine's two
flags decide what it sends (slice-0112 decoupled them). `setSolicitArmed(g_solicitArmed)` is
called beside the existing `setDeauthArmed(g_deauthArmed)`. A `[SOLICIT] ARMED`/`disarmed`
line announces the arm at begin (like `[DEAUTH]`), and a 2 s `[SOLICIT] txOk=/txFail=`
heartbeat, gated on `g_solicitArmed`, is emitted in `huntLoopPump` from the engine's
`solicitTxOk()`/`solicitTxFail()` tallies (slice-0112) so a disarmed binary stays silent on it
and the on-air verify has a repeating line to gate on (the S3 USB-CDC may miss a boot-once
line, the slice-0028 lesson the deauth heartbeat already encodes). The heartbeat constant
reuses `kDeauthHeartbeatMs` (2 s) rather than a second magic value.

**What was deliberately not reused:** the deauth heartbeat's own due-timer (`g_deauthHbDueMs`)
is not shared; the solicit heartbeat gets its own `g_solicitHbDueMs` so arming only solicit
does not depend on the deauth timer having advanced, and the two lines stay independently
paced. They share only the interval constant.

## Verification

Scenarios 1-3 are host-assertable on the native lane (ADR-0004 lane 1). Scenario 4 is the
on-air TX leg that no unit test can assert (§3): it ships a device verify script, wired into
`package.json` (R11), run by the operator on attached hardware, never in cloud CI (§4 #5).

- `test/test_provisioning_store` (extended): `pmkidSolicit` round-trips true when set and
  false by default, and a raw NVS triad with no `pmkid_arm` key loads it false (the pre-field
  upgrade). Covers DoD scenario 1. Mirrors `test_deauth_arm_roundtrips_and_defaults_off` +
  `test_deauth_absent_key_loads_disarmed`.
- `test/test_provisioning_form` (extended): the `pmkid` checkbox renders `checked` from a set
  model and unchecked from an unset one (no secret echoed), and `resolveProvisioningUpdate`
  takes the arm from the submission (a stored-on, submitted-off record disarms). Covers DoD
  scenario 2.
- `scripts/0113-solicit-verify.mjs`, wired as `npm run verify:solicit`: runs the **shipped**
  (non-test-hooks) classic-ESP32 or S3 binary on an attached, provisioned, solicit-armed
  device and gates on a repeating `[SOLICIT] txOk=<N>` heartbeat with N>0 and no
  `[FATAL]`/`[ERROR]`, writing the serial log to `artifacts/`. Covers DoD scenario 4. The
  `hunt_loop` arm wiring itself is review-only device glue (§4 #16/#47), proven on air by this
  script. Scenario 3's begin-time wiring is observable in the same run's `[SOLICIT] ARMED`
  boot line.

## As built

Shipped as designed, no unforeseen gate. `ProvisioningRecord` gained `bool pmkidSolicit`;
`provisioning_store.cpp` added the `pmkid_arm` NVS key (read default-false, written into the
all-or-nothing `wrote` conjunction). `SetupFormModel` gained `pmkidSolicit`, and
`buildSetupForm` renders a `name='pmkid'` checkbox beside the deauth arm with an
authorized-use-only help note; `resolveProvisioningUpdate` took no code change (the toggle
rides the `out = submitted` copy) beyond its doc comment. Both portals parse `arg("pmkid")`
at save and set `model.pmkidSolicit` at render. `hunt_loop` widened the transmitter injection
to `(g_deauthArmed || g_solicitArmed)`, added `g_solicitArmed`/`g_solicitHbDueMs`, called
`setSolicitArmed`, and emits the `[SOLICIT]` arm line + 2 s txOk/txFail heartbeat (sharing
`kDeauthHeartbeatMs`). `scripts/0113-solicit-verify.mjs` is wired as `verify:solicit`.

Scenarios 1-3 proved on the native lane: `test_provisioning_store` (+2: arm round-trips and
defaults off, absent key loads off), `test_provisioning_form` (+2: the checkbox renders
checked/unchecked, the arm merges from the submission). Scenario 4 is the operator's on-air
run (`verify:solicit`), the armed-solicitation TX leg. The full native lane and both
tight-budget classic-ESP32 device builds stay green. This closes ADR-0110 #47 clause (d) and
the on-air half of (b)/(c), and deletes the LEDGER clientless-PMKID umbrella line.
