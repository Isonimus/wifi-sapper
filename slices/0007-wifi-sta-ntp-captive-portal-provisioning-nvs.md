---
id: '0007'
title: "WiFi STA, NTP time sync, and captive-portal provisioning to NVS"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Give a freshly-flashed Sapper the two secrets it needs before it can do anything useful —
**WiFi station credentials** and the **wpa-sec API key** — and get it onto the network with a
correct clock, all without assuming a screen, keyboard, or SD card. This is the concrete
implementation of ADR-0006: a first-boot **captive AP portal** (SoftAP + DNS hijack + a
one-page web form) is the primary provisioning path, secrets land in **NVS** through a single
`persistProvisioning()` seam, and a **two-way boot gate** sends a provisioned device straight
to STA → NTP → the (future) engine while an unprovisioned or unreachable one falls back to the
portal. It carries forward the WiFi-STA-connect and NTP patterns the parent Adversary already
proved, but replaces its SD/LittleFS-file + on-screen-keyboard config path — both assume
surfaces the headless Sapper lacks.

This delivers the `LEDGER.md` `slice-2` feature item (ADR-0006). It does **not** close the two
deferrals ADR-0006 recorded — flash-encryption for secrets at rest, and the optional
SD/LittleFS seed path — which stay open in the ledger. It also makes CLAUDE.md §4 invariant #3
(every pre-engine blocking state pumps the serial channel) concrete for the first time: the
provisioning state is that blocking state, so this slice is where #3 moves from `pending
(LEDGER)` to verified.

## Definition of Done

**Scenario A — credential validation, the boot gate, and SoftAP naming are unit-tested.**
- **Given** the `native` env, with no board attached
- **When** `npm run test:native` runs
- **Then** the pure credential validator rejects an empty SSID, an SSID over 32 bytes, a
  WPA2 passphrase outside 8–63 bytes (empty is accepted only for an open network), and an
  empty wpa-sec key, and accepts a well-formed triad; the boot gate returns
  `Phase::Provisioning` for absent-or-invalid stored credentials, for valid credentials once
  the STA-failure count has reached its bound, and when re-provisioning was requested, and
  returns `Phase::StationConnect` only for valid stored credentials with no prior failure and
  no re-provision request; and `softApSsid()` derives `Sapper-XXXX` from the last two bytes of
  a given MAC.

**Scenario B — the firmware builds in both the shipped and test-hooks configurations.**
- **Given** the `cardputer` env, built once normally and once with `SAPPER_TEST_HOOKS` defined
- **When** `npm run build:boards` runs each configuration
- **Then** both compile and link to a `.elf` with no error, and the `SAPPER_TEST_HOOKS`
  configuration's compile-time credential injection reaches NVS only by calling
  `persistProvisioning()` — the same seam the web form uses — with no serial command, in either
  build, able to write or clear credentials (CLAUDE.md §4 invariants #6, #7; verified at review,
  not by the compiler).

**Scenario C — an unprovisioned device raises the portal and stays observable.**
- **Given** a Cardputer with its NVS `sapper` namespace erased (via esptool before flash, not
  over serial — no serial credential path exists) and flashed with this slice's firmware
- **When** it boots and the verify script sends `ping` then `state`
- **Then** no `[FATAL]` line appears, the device raises a SoftAP whose SSID matches
  `Sapper-XXXX`, `[STATE]` reports `phase=provisioning`, and the channel answers `[CMD] pong`
  and a well-formed `[STATE]` across repeated reads without the phase advancing — evidencing
  that the pre-engine provisioning state pumps the serial channel (ADR-0003 §7; CLAUDE.md §4
  invariant #3, the instance that flips it to verified).

**Scenario D — a provisioned boot associates and syncs time through the NVS seam.**
- **Given** a `SAPPER_TEST_HOOKS` build compiled with a reachable network's credentials
  supplied through environment variables (never committed) and injected at boot through
  `persistProvisioning()`
- **When** the device boots and the verify script polls `state`
- **Then** the reported phase advances `provisioning`→`station_connect`→`time_sync`→`ready`,
  `[STATE]` shows the STA obtained an IP and the clock advanced past a year-2020 sentinel, and
  the credentials reached NVS only via `persistProvisioning()` — with no browser and no serial
  write — evidencing the ADR-0006 decision-#7 verify seam and the NTP-depends-on-STA-depends-on
  -provisioning chain.

## Design

**Boot state machine and the `phase` field.** slice-0005 shipped a `[STATE]` line whose
`phase=bringup` is a literal string; this slice makes `phase` a real runtime value. A small
`Phase` enum (`Provisioning`, `StationConnect`, `TimeSync`, `Ready`) is owned by the boot
sequence in `main.cpp` and *read* by `SerialChannel::reportState()`. The superloop calls
`SerialChannel::pump()` on every iteration regardless of phase, so every pre-engine state —
not just provisioning — satisfies invariant #3 uniformly; that is the simplest thing that
works (KISS) and avoids a per-state pump call each new state would have to remember. The
channel gains **no** new command: `phase` is a new field on the existing read-only `[STATE]`
line, so the observation vocabulary stays exactly `ping`/`state`/`dump` and invariant #1
(observation mutates nothing) is untouched.

**The provisioning store and the single seam.** Credentials live in NVS via Arduino
`Preferences` under a `sapper` namespace (`wifi_ssid`, `wifi_pass`, `wpasec_key`), per ADR-0006
decision #2. Two thin device functions wrap `Preferences`: `loadProvisioning()` reads the
triad at boot, and **`persistProvisioning(creds)` is the only writer** (CLAUDE.md §4 invariant
#6). The Adversary's `SettingsManager` (SD/LittleFS JSON at `/adversary/config/*.txt`,
`../adversary/src/modules/storage/settings_manager.h`) was considered and **deliberately not
reused**: it assumes removable storage and its values are typed in through the on-screen
keyboard — both the interactive-device assumptions ADR-0006 rejects. The Adversary does not use
NVS at all, so there is no store to inherit; NVS is new here and is the platform-standard home
for WiFi credentials that needs no SD assumption and survives a reflash.

**Validation and the boot gate are pure, so they are host-tested.** The form fields and the
gate decision are extracted as pure functions with no `Preferences`, `WiFi`, or `Serial`
dependency: `validateCredentials(fields)` (the 802.11 SSID ≤ 32 bytes and WPA2 8–63-byte
passphrase bounds, wpa-sec key non-empty — fail loud on any violation, no silent default),
`decideBootPhase(stored, staFailCount, reprovisionRequested)`, and `softApSsid(mac)`. This is
the same cut slice-0005 used — pure parser tested natively, hardware behaviour proved by the
verify script — and it keeps the device-only surface as small as the NVS/WiFi/HTTP calls that
genuinely need hardware. The `Preferences` wrapper itself is not unit-tested behind a fake: a
speculative fake NVS abstraction would be exactly the over-abstraction the quality bar forbids
before a second caller exists (rule of three); its behaviour is proved on hardware by Scenarios
C and D.

**Captive portal.** Reuses the Arduino `DNSServer` + `WebServer` + SoftAP plumbing shape the
Adversary proved in `../adversary/src/modules/ap/arduino_captive.h` (`start()` /
`handleRequests()` / a credential callback), per ADR-0006 decision #1 — but **not** that file's
`ArduinoCapturedCredential` harvesting or its offensive portal-page templates, which serve a
different purpose entirely. The Sapper portal serves one config form (SSID + password +
wpa-sec key); its POST handler validates via `validateCredentials()` and, only on success,
commits via `persistProvisioning()`, then reboots into the gate. The SoftAP SSID is
`softApSsid(mac)` (`Sapper-XXXX`, last two MAC bytes) with a documented default password
(ADR-0006 decision #5), so a screenless board is still provisionable by an operator who has
read the README.

**WiFi STA + NTP.** The connect-with-clean-state pattern is adapted from the Adversary's
`WiFiConnection::connect()` (`../adversary/src/modules/wifi/wifi_connection.cpp` — clear
promiscuous mode, `WiFi.disconnect(true)`, then `WiFi.begin`), and NTP from its
`TimeManager::syncFromNTP()` (`configTime(...)` against `pool.ntp.org`,
`../adversary/src/modules/system/time_manager.h`). Both are adapted, not linked: they are
entangled with the Adversary's event bus, toast UI, and SD time-cache, none of which this
headless slice carries. A bounded STA-association retry count feeds `decideBootPhase()` so a
device that cannot reach its configured network re-opens the portal rather than looping
silently (ADR-0006 decision #3; quality bar §3, fail loud). NTP sync gates `Ready` on the clock
advancing past a year-2020 sentinel — an unsynced epoch-0 clock would silently break the TLS
certificate validity window every wpa-sec upload depends on, so `time_sync` is a real phase, not
a fire-and-forget call.

**Re-provisioning without a serial actuation path.** On profiles that declare an input
(`BoardProfile::input != InputKind::None`), a GPIO/button hold at boot clears NVS and forces
the portal; on profiles with no input, the STA-failure fallback of `decideBootPhase()` is the
only re-provision route (ADR-0006 decision #6). **No serial command clears or writes
credentials in any build** (CLAUDE.md §4 invariant #7) — which is also why the test harness
cannot inject over serial and instead uses the compile-time seam below.

**The `SAPPER_TEST_HOOKS` injection is compile-time, not serial.** ADR-0006 decision #7 says the
harness injects credentials through the `persistProvisioning()` seam; invariant #7 forbids any
build's *serial* vocabulary from writing credentials. These reconcile by making the injection a
**compile-time** one: under `SAPPER_TEST_HOOKS`, credentials passed as build flags are handed to
`persistProvisioning()` once at boot, driving a provisioned boot through the exact seam a real
form POST uses (ADR-0003 decision #3: stimulus enters through the seam a real event uses) with
no new serial command and no parallel test-only write path. A future operator who reaches for a
`provision` serial command under `SAPPER_TEST_HOOKS` should stop here: invariant #7 is why it is
a build flag instead. The build-flag values come from the environment at build time and are
never committed.

## Verification

- **Native Unity tests** (Scenario A) — `test/test_provisioning` covering
  `validateCredentials` (each rejection boundary and the accepting case), `decideBootPhase`
  (every gate branch), and `softApSsid` (MAC → `Sapper-XXXX`). Wired as `npm run test:native`
  (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1).
- **Board compile** (Scenario B) — `npm run build:boards` (`pio run -e cardputer`) links the
  shipped `.elf`; the `SAPPER_TEST_HOOKS` configuration is built with
  `pio run -e cardputer` under that flag. Runs in cloud CI (ADR-0004 lane 2). The invariant-#6/#7
  claims about *which* code path reaches NVS are review-only, as CLAUDE.md §4 declares them —
  the compiler proves the link, `/wrap-up`'s adversarial pass proves the boundary.
- **Device verify** (Scenarios C, D) — `scripts/0007-provisioning-verify.mjs`, wired as
  `npm run verify:provisioning`. Its unattended, machine-checkable core (Scenario C) needs no
  access point: it erases the `sapper` NVS namespace, flashes the shipped build, and asserts no
  `[FATAL]`, a `Sapper-XXXX` SoftAP, `phase=provisioning`, and `ping`/`state` answered without
  the phase advancing — this half runs the invariant-#3 proof. Its attended half (Scenario D)
  reads a reachable network's credentials from the environment (`$SAPPER_TEST_WIFI_SSID`,
  `$SAPPER_TEST_WIFI_PASS`, `$SAPPER_TEST_WPASEC_KEY` — never committed), builds and flashes the
  `SAPPER_TEST_HOOKS` configuration with them, and asserts the phase advances through
  `station_connect` and `time_sync` to `ready` with an IP and a clock past the year-2020
  sentinel; it writes the captured phase progression to `artifacts/0007-provisioning.state.txt`
  for human review and is skipped with a printed notice when those environment variables are
  absent. Runs on attached hardware only, never cloud CI (ADR-0004 lane 3, invariant #5).

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each Definition-of-Done
scenario was met or recording the deviation.)_
