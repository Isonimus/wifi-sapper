---
id: '0030'
title: "Autonomous in-loop deauth armed by a default-off provisioning toggle"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Ship the appliance's intended capability: the hunt loop forces handshakes by deauthing each AP it
parks on. Per ADR-0029: inject an optional `RawTransmitter` into the `HuntEngine` so its Capturing
phase transmits broadcast deauth/disassoc at `capturingBssid()` on the already-parked channel;
compile the raw-TX capability into the shipped binary; and gate *transmitting* behind a default-off
provisioning toggle (`ProvisioningRecord.deauthEnabled`, a captive-portal checkbox) so a shipped
appliance deauths only when the operator has explicitly armed it. This **supersedes ADR-0027 decision
3 / invariant #15's compile-gate** — the capability moves from `SAPPER_TEST_HOOKS`-absence to a
runtime, operator-armed, default-off check.

## Definition of Done

**Scenario A — an armed engine deauths the current target during Capturing (host).**
- **Given** a `HuntEngine` constructed with a `FakeRawTransmitter`, one AP discovered and being captured
- **When** `tick()` runs during the Capturing phase
- **Then** the transmitter receives a broadcast deauth frame: byte 0 = `0xC0`, Addr1 (bytes 4–9) =
  the broadcast address, Addr2/Addr3 (bytes 10–21) = the captured AP's BSSID (`capturingBssid()`),
  and `deauthTxOk()` climbs — the engine forces the handshake at the AP it is already tuned to.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario B — each burst is a deauth AND a disassoc (host).**
- **Given** the same armed, capturing engine
- **When** one deauth burst is transmitted
- **Then** the transmitter receives two frames — one `ManagementSubtype::Deauth` (`0xC0`) and one
  `Disassoc` (`0xA0`) — both broadcast at the current BSSID (ADR-0029 #1).
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario C — deauth respects the cadence, leaving listen gaps (host).**
- **Given** an armed engine with a known `deauthIntervalMs`, in Capturing
- **When** `tick()` is called repeatedly within one interval, then past it
- **Then** exactly one burst is transmitted per interval — not one per tick — so the client has a gap
  to re-associate and complete the handshake the deauth is meant to force (ADR-0029 #1).
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario D — a disarmed engine never transmits (host). The default-off safety gate.**
- **Given** a `HuntEngine` constructed with a `nullptr` transmitter (the default)
- **When** it runs a full discover→capture→advance cycle, including Capturing
- **Then** no frame is ever transmitted — a disarmed or unprovisioned device is a passive hunt,
  exactly as before ADR-0029 (invariant #16; the safety property a lost/misused unit depends on).
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario E — deauth fires only in Capturing, and stops once the handshake is captured (host).**
- **Given** an armed engine
- **When** it is Discovering (before any target), and again after a capture completes (the phase
  leaves Capturing for Quiescing)
- **Then** no deauth is transmitted outside the Capturing phase — the engine deauths the AP it is
  actively capturing, and stops the moment that capture succeeds or the window ends.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario F — the arm flag round-trips through NVS and defaults off (host).**
- **Given** a `ProvisioningRecord` with `deauthEnabled` set, persisted; and separately a record
  written before the field existed (the key absent from NVS)
- **When** it is loaded back
- **Then** the armed record loads armed, and the record with the key absent loads **disarmed**
  (`getBool` default false) — an upgrade or an unchecked box never silently arms (ADR-0029 #3).
- **Proof:** `npm run test:native` → `test/test_provisioning_store`.

**Scenario J — on-air gate: a SHIPPED binary transmits deauth in its hunt loop (device).**
- **Given** the **`cardputer` (shipped, non-hooks) binary**, provisioned via the captive portal with
  the **Enable deauth** box checked, an authorized AP on air
- **When** the hunt loop discovers the AP and enters Capturing
- **Then** the device logs `[DEAUTH] ARMED` and a repeating `[DEAUTH] txOk=<N>` heartbeat with **N>0**
  and no `[FATAL]`/`[ERROR]` — proof the raw-TX capability is present *and active in a shipped binary*,
  which invariant #15's old gating clause forbade. This is the pass/fail gate.
- **Proof:** `npm run verify:deauth-loop` (`scripts/0030-deauth-loop-verify.mjs`); artifact
  `artifacts/0030-deauth-loop-tx.txt`.

**Scenario K — on-air opportunistic e2e: the loop autonomously captures a forced handshake (device).**
- **Given** the armed shipped loop pointed (by discovery) at an authorized AP with a connected client
- **When** the forced client re-associates during the run and its 4-way handshake is sniffed
- **Then** the loop captures it, enqueues it, and (STA permitting) uploads it to wpa-sec — the fully
  autonomous force→capture→upload path. Environmental (a client must reconnect), so **not** the gate;
  each stage is already proven (slice-0018 upload, slice-0028 forced capture).
- **Proof:** `npm run verify:deauth-loop` when a client is present; observed in the device log.

## Design

Per ADR-0029. Changes:

- **`net/hunt_engine.{h,cpp}`** — the ctor gains a trailing optional `RawTransmitter* transmitter =
  nullptr`; `HuntConfig` gains `deauthIntervalMs` (default 2000ms). A private `maybeTransmitDeauth()`,
  called only from the Capturing branch of `tick()` while still capturing, builds (via the pure
  `buildDeauthFrame`) and transmits a broadcast deauth + disassoc at `capturingBssid()` on the
  cadence; `doStartCapturing` arms the cadence to fire immediately. `deauthTxOk()`/`deauthTxFail()`
  expose the counts. A `nullptr` transmitter transmits nothing (the default; invariant #16).
- **`net/raw_transmitter_esp32.{h,cpp}`** — the `#if` guard drops `&& defined(SAPPER_TEST_HOOKS)`, so
  the `Esp32RawTransmitter` and the SDK bypass compile into every device build (ADR-0029 #2). The
  `weaken_deauth_pre.py` pre-script already runs for the shipped env, so the weak-symbol override
  links with no new "multiple definition" risk.
- **`net/provisioning_store.{h,cpp}`** — `ProvisioningRecord` gains `bool deauthEnabled`; the store
  reads it with `getBool("deauth_arm", false)` (default-off) and writes it in the all-or-nothing
  triad persist. `native/mock/Preferences.h` gains `getBool`/`putBool` so the round-trip is
  host-tested (ADR-0008 rule of three: the store now uses them).
- **`net/captive_portal.cpp`** — the setup form gains an **Enable deauth** checkbox (unchecked by
  default) with an authorized-use caption; `handleSave` sets `deauthEnabled` from it.
- **`hunt_loop.cpp`** — a file-scope `Esp32RawTransmitter`; the engine is placement-new'd in
  `huntLoopBegin` once `creds.deauthEnabled` is known, injected with the transmitter iff armed (else
  `nullptr`), matching the file's existing runtime-constructed singletons; a boot log line
  (`[DEAUTH] ARMED` / `disarmed`) and, when armed, a 2s `[DEAUTH] txOk=… txFail=… capturing=…`
  heartbeat for a surface and the on-air verify.
- **`scripts/0030-deauth-loop-verify.mjs`** (`npm run verify:deauth-loop`) — drives the **shipped
  `cardputer`** binary (not testhooks); gates on the repeating `[DEAUTH] txOk>0` heartbeat with
  `[DEAUTH] ARMED` and no `[FATAL]`/`[ERROR]`; saves `artifacts/0030-deauth-loop-tx.txt`.
  **AUTHORIZED USE ONLY** — a shipped, armed appliance deauths every AP it discovers.

## Verification

- **Scenarios A–E** (engine policy) and **F** (arm round-trip): native Unity lane (ADR-0004 lane 1),
  `npm run test:native` → `test/test_hunt_engine` (against `FakeRawTransmitter` + the fake clock) and
  `test/test_provisioning_store` (behind the fake `Preferences`).
- **Scenario J** (on-air gate) and **K** (on-air opportunistic e2e):
  `scripts/0030-deauth-loop-verify.mjs` (`npm run verify:deauth-loop`), driving the shipped
  `env:cardputer` binary provisioned with deauth armed; fails on any `[FATAL]`/`[ERROR]`; artifact
  `artifacts/0030-deauth-loop-tx.txt`.
- Board compile (ADR-0004 lane 2): `pio run -e cardputer` and `-e cardputer_testhooks`.

## As built

Shipped as designed (ADR-0029), with these deviations and results worth recording:

- **The default flipped from on to OFF during design.** The operator's first instinct was a
  default-*on* arm toggle (the appliance's purpose is to attack, so make attacking the default); the
  §2 counter — a lost or misused unit becomes a default-on jammer with no opt-in, illegal outside an
  authorized engagement — was accepted, and the default became **off with an explicit arm**. ADR-0029
  records both the decision and the position it replaced. This is the one substantive change from the
  pre-slice plan (the LEDGER had said "default-on").
- **ADR-0027 was superseded in part, in prose, not by frontmatter.** Only its decision 3 (the
  `SAPPER_TEST_HOOKS` compile-gate) is lifted; its pure-builder, raw-bytes-seam, and proof decisions
  still govern, so ADR-0027 keeps `status: accepted` and ADR-0029 states the partial supersession in
  its Decision text. Marking 0027 wholly superseded would have misrepresented its still-live decisions.
- **Measured cost:** both board envs compile. Shipped `cardputer` Flash **40.7%** (1,359,479 bytes) /
  RAM 35.3% — up from slice-0028's 40.6%; that small delta *is* the raw-TX path + the engine's deauth
  logic now present in the *shipped* binary. The `weaken_deauth_pre.py` pre-script already ran for this
  env, so the strong `ieee80211_raw_frame_sanity_check` override linked into the shipped binary with
  **no "multiple definition" error** — the reviewer independently confirmed the gate is now runtime,
  not compile-time, and §4's #15/#16 match the code.
- **Host lane: 13 `test_hunt_engine` + 7 `test_provisioning_store`, all passing** (g++ with
  `-Wall -Wextra`, no warnings; `pio` native is broken here — the established workaround). The engine
  tests cover armed-broadcast-at-target, deauth+disassoc per burst, cadence-not-per-tick,
  disarmed-never-transmits (the invariant #16 safety gate), rejected-frames-count-as-txFail, and
  deauth-only-while-Capturing-and-stops-on-capture; the store tests cover the arm round-trip, default-
  off, and default-off-on-absent-key (an upgraded device is never silently armed).
- **On-air Scenario J PASSED on the SHIPPED binary.** `artifacts/0030-deauth-loop-tx.txt`: the armed
  `cardputer` (non-hooks) build transmitted deauth during its hunt loop — **`txOk` climbing 282→288,
  `txFail=0`**, `capturing` toggling 1/0 across Discovering/Capturing. `txOk` began at 282, not 0,
  because the device was already running when the verify attached (the S3 USB-CDC late-attach), which
  is exactly why the gate is the *repeating* heartbeat, not a boot-once line (the slice-0028 lesson,
  reused). This is the evidence invariant #15's old gating clause forbade: a *shipped* binary putting
  deauth on the air.
- **Scenario K did not trigger this run:** no client reassociated during the window, so the fully
  autonomous force→capture→upload e2e was not exercised. Each stage is proven elsewhere (slice-0018
  upload, slice-0028 forced capture); it is opportunistic and environmental, never the gate.
- **Adversarial review (blind, Sonnet 5) — 3 findings, 0 correctness/invariant, all addressed;** the
  cadence/wrap-safety math, the `hunt_loop` placement-new lifecycle ordering, the NVS bool round-trip
  (partial-write-wipe and default-off-on-absent-key), and the native-mock `getBool`/`putBool` semantics
  were all independently traced and confirmed correct.
  1. **Test-gap:** the `deauthTxFail_` branch had no coverage, though the on-air heartbeat reads it.
     **Fixed** — `test_rejected_frames_count_as_txfail_not_txok` sets `FakeRawTransmitter.nextResult =
     false` and asserts a rejected burst lands in `deauthTxFail()`, not `deauthTxOk()` (a swapped
     counter now fails the suite).
  2. **Stale doc:** `deauth.h`'s header still called the raw-TX seam `SAPPER_TEST_HOOKS`-gated, false
     as of this slice. **Fixed** — corrected to the runtime-arm wording (every other mention was
     already updated; this one file was missed).
  3. **Portal re-save disarms silently:** the captive-portal form is stateless, so re-opening it and
     saving re-enters all fields — an unchecked box disarms (fails safe, and mirrors the pre-existing
     `webhookUrl` behaviour). **Handled by documentation + a deferral**, not a code change: a dynamic
     form that echoed the stored `webhookUrl` back would leak that secret into served HTML, so the
     verify precondition now states re-save re-enters all fields, and the LEDGER carries the
     render-the-checkbox-without-echoing-secrets fix.
- **Deferrals recorded** (LEDGER): deauth-cadence hardware tuning (`deauthIntervalMs` is an
  architecture-derived 2000ms, not yet measured); per-BSSID allow/deny scoping for mixed environments;
  and the stateless-portal-form fix from finding 3.
