---
id: '0029'
title: "Autonomous in-loop deauth: an injected raw-TX seam armed by a default-off provisioning toggle"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

ADR-0027 built the deauth **mechanism** — a pure `buildDeauthFrame`, a thin `RawTransmitter` seam,
and a device `Esp32RawTransmitter` — and proved it on air, but deliberately kept it behind the bench
gate: its decision 3 compiled the raw-TX capability *only* under `SAPPER_TEST_HOOKS`, so no shipped
binary could transmit a deauth frame (§4 invariant #15's gating clause). That decision named its own
successor: *"when the LEDGER's allowlist-gated unattended-deauth operating mode is built, its ADR will
supersede this decision's gating … and must state why loosening the gate is safe then."* This is that
ADR.

The appliance's purpose settles the direction. ADR-0003 already records that *"the Sapper's entire
purpose is an autonomous transmit path (deauth)"*, and the operator's stated deployment is industrial
pentest: every network in the authorized factory is a target, so the hunt loop should force
handshakes by deauthing each AP it parks on. A capability that exists only in a bench build cannot
fulfil that purpose — the shipped appliance must deauth in its loop.

The open question was never *whether* to ship deauth but *how to gate it once it ships*. Two failure
modes bound the choice:

- **Under-gating.** A shipped binary that deauths unconditionally is a rogue jammer the moment the
  device leaves its authorized environment — lost, reused elsewhere, or flashed by someone without an
  engagement. Deauthing networks you are not authorized to test is illegal in most jurisdictions, and
  a firmware that does it *by default* offers no opt-in and no record of operator intent.
- **Over-gating.** Keeping the compile-time `SAPPER_TEST_HOOKS` gate (or shipping two firmware
  variants, a passive one and an attack one) means the one thing the appliance exists for requires a
  special build, defeating a field-reconfigurable appliance and leaving the shipped binary unable to
  do its job.

The resolution keeps the capability in every binary but makes *transmitting* a deliberate operator
act, defaulting to off. The design was reviewed against §2 (the operator may be wrong): the operator's
first instinct was a default-*on* toggle (the appliance's purpose is to attack, so make attacking the
default); the counter — a lost or misused unit becomes a default-on jammer with no opt-in — was
accepted, and the default was flipped to **off with an explicit arm**. That is recorded here as the
decision, per §2's rule that a considered position becomes a decision record, not a silent choice.

The seams this rests on already exist and were measured in slice-0028:

- The `HuntEngine` (ADR-0015) parks the radio on the target's channel throughout its **Capturing**
  phase and exposes the target via `capturingBssid()`, so autonomous deauth needs **no channel
  inference** — the engine is already tuned where the deauth must go. `tick()` runs on the app task,
  the only safe context to call `esp_wifi_80211_tx` (the driver-callback `onFrame` may not, §4
  invariant #10).
- The `weaken_deauth_pre.py` pre-script already runs for the shipped `cardputer` env (it lives in
  `[env_embedded]`), so `ieee80211_raw_frame_sanity_check` is *already* weak in shipped builds; today
  the gated `raw_transmitter_esp32.cpp` simply redefines nothing. Lifting the gate is therefore mostly
  a matter of letting that translation unit compile into shipped builds — no new build plumbing.
- Provisioning (ADR-0006) already carries an *optional, separately-validated* field beside the
  credential triad — the webhook URL — which is the exact shape a boolean arm toggle takes.

## Decision

**1. The `HuntEngine` takes an optional, injected `RawTransmitter*` (default `nullptr`) and deauths
only while Capturing.** During the Capturing phase, and only there, `tick()` transmits a broadcast
deauth **and** disassoc frame (`ManagementSubtype::Deauth`/`Disassoc`, dest = `kBroadcastMac`, source
and BSSID = `capturingBssid()`, reason `Class3FrameFromNonassoc`) on a cadence,
`HuntConfig.deauthIntervalMs`. A `nullptr` transmitter is the pre-0029 behaviour unchanged — a purely
passive hunt. The engine still *builds* frames only through the pure `buildDeauthFrame` (§4 invariant
#15's construction-purity clause is untouched); it adds only the *policy* of when to transmit, which
is host-testable through an injected `FakeRawTransmitter` and the fake clock (ADR-0004 lane 1), like
the rest of the state machine. The engine exposes `deauthTxOk()`/`deauthTxFail()` counters for a
surface and the on-air verify to read, mirroring `parkedChannel()`/`capturingBssid()`.

The cadence leaves listen gaps: a deauthed client re-associates on a seconds scale, and the 4-way
handshake we are trying to capture only completes *after* it re-associates, so deauthing continuously
would starve the very capture it exists to force. `deauthIntervalMs` defaults to a conservative 2000ms
(one burst, then ~2s to re-associate and hand-shake, across the 8s capture window) — an
architecture-derived floor, not a measurement; the LEDGER carries the hardware-tuning item, exactly as
`dwellMs` and `settleMs` do (ADR-0013, ADR-0015).

**2. The raw-TX capability ships in every device binary.** `raw_transmitter_esp32.{h,cpp}` drops its
`SAPPER_TEST_HOOKS` guard and compiles under `#if !defined(UNIT_TEST)` — i.e. in the shipped
`cardputer` build as well as the bench build. The `esp_wifi_80211_tx` path and the
`ieee80211_raw_frame_sanity_check` bypass are therefore present in the shipped appliance. This is the
half of ADR-0027 decision 3 that this ADR **supersedes**: the gate moves from *compile-time absence*
to a *runtime, operator-armed* check (decision 3 below). ADR-0027's other decisions — the pure builder
(1), the raw-bytes seam (2), the mechanism/proof scope of that slice (4), and the determinism split of
its proof (5) — remain correct and in force; this ADR does not touch them, which is why ADR-0027 keeps
`status: accepted` rather than being marked wholly superseded. Superseding one ADR's frontmatter
would misrepresent its still-governing builder and seam decisions as withdrawn.

**3. Transmitting is gated at runtime by a default-off provisioning toggle.**
`ProvisioningRecord` gains a boolean `deauthEnabled`, persisted through the `persistProvisioning()`
NVS seam (§4 invariant #6) and set from a captive-portal checkbox — never from a serial command (§4
invariant #7 holds; the arm is provisioning, not actuation, so §4 invariant #4's "no shipped binary
can be *commanded to deauth over serial*" is untouched). It defaults to **off**: `loadProvisioning()`
reads it with `getBool(key, /*default=*/false)`, so a device provisioned before this field existed, or
one whose operator left the box unchecked, is disarmed. `hunt_loop` injects the real
`Esp32RawTransmitter` into the engine **only** when `deauthEnabled` is set; otherwise it injects
`nullptr` and the shipped loop hunts passively. The result: the capability is in the binary, but a
lost, misused, or merely default-provisioned unit transmits no deauth frame — arming is always a
deliberate operator act, recorded in NVS.

**4. Scope: broadcast deauth at every AP the hunt captures.** The engine deauths the broadcast address
(every client of the parked AP at once), at whatever AP its round-robin is currently capturing — the
whole authorized factory, per the operator's deployment. It needs no station scanner (broadcast
reaches unknown clients) and no per-AP configuration. **Per-BSSID allow/deny scoping** — sparing named
networks in a mixed environment — remains a separate LEDGER item, layered on top when an environment
needs some networks left alone; it is not required for an all-authorized factory and is not pulled
forward here.

**5. Proof splits by determinism (ADR-0004 §3), and the on-air half runs on the SHIPPED binary.** The
engine's deauth *policy* — transmits only while Capturing, broadcast at the current target, respects
the cadence, and **never transmits when disarmed** (the default-off safety gate) — is host-tested with
a `FakeRawTransmitter` and the fake clock, where it is deterministic. The provisioning round-trip
(arm persists; an absent key or pre-existing record reads disarmed) is host-tested behind the fake
`Preferences`. The on-air verify (`scripts/0030-deauth-loop-verify.mjs`) runs the **`cardputer`
(shipped, non-hooks) binary**, provisioned with deauth armed, and gates on the shipped loop
transmitting real deauth frames during its hunt — a repeating `[DEAUTH] txOk>0` heartbeat with
`[DEAUTH] ARMED` and no `[ERROR]`/`[FATAL]`. That is the evidence that matters for this ADR: a
*shipped* binary now transmits deauth, which is precisely what invariant #15's old gating clause
forbade. The forced-handshake→capture→upload end-to-end is opportunistic (a client must re-associate
during the run) and each stage is already proven (slice-0018 upload, slice-0028 forced capture), so it
is observed-if-present, never the gate.

## Consequences

- **CLAUDE.md §4 is updated in the same commit.** Invariant #15 loses its `SAPPER_TEST_HOOKS`
  compile-gate clause and keeps only its enduring construction-purity + raw-bytes-seam claims (still
  true, still mirroring #8/#9). A new **invariant #16** records the runtime arm gate: the capability
  ships in every binary but transmits only when armed via the default-off provisioning toggle, and a
  disarmed/unprovisioned device transmits nothing. #16 is verified by the on-air verify (shipped-armed
  TX) and the host tests (disarmed engine emits nothing); the portal/`hunt_loop` wiring half is
  review-only.
- **§4 invariant #4 (ADR-0003) is untouched and re-affirmed.** Autonomous deauth is engine-driven, not
  a serial command; no serial actuation vocabulary is added, and the arm is a provisioning field, not a
  serial path. "No shipped binary can be *commanded to deauth over serial*" stays true.
- **The shipped `cardputer` binary now links the raw-TX path.** Its flash cost is recorded in
  slice-0030's `## As built` from a measured build, not estimated. `weaken_deauth_pre.py` already ran
  for this env, so no new "multiple definition" link risk is introduced — the strong override simply
  now has a shipped consumer.
- **A default-off toggle means the appliance ships inert for deauth and must be armed per deployment.**
  This is the intended safety posture, not a regression: a fresh flash, or a re-provision that leaves
  the box unchecked, hunts passively. The README's provisioning guide gains the arm checkbox and an
  authorized-use warning.
- **The LEDGER's autonomous-deauth item is discharged by this ADR/slice**; two follow-ups remain on
  it: hardware-tuning `deauthIntervalMs` against a measured capture rate, and the per-BSSID allow/deny
  scoping for mixed environments.
