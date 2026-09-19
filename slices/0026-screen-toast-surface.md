---
id: '0026'
title: "On-device status-HUD + cracked-toast display surface"
type: slice
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Goal

Turn the Cardputer panel from a static boot splash into a live status surface. The third slice-7
surface (after the LED and the webhook) and the second *passive* one: it observes engine facts on the
event bus (ADR-0021) and renders them on its own `tick()`, needing no network and none of the webhook's
window-scoped machinery (ADR-0025). It draws a persistent HUD — what the appliance is doing, how many
captures have uploaded, how many passwords have cracked, whether the last hourly sync succeeded — and
pops a transient **CRACKED** banner naming the ESSID + BSSID (never the PSK) over the HUD on each new
password. The screen says *what* cracked, which the one-colour LED (slice-0022) cannot.

## Definition of Done

**Scenario A — the HUD status follows the drain (host).**
- **Given** a `ScreenToastSurface` subscribed to a bus
- **When** `DrainStarted` then `DrainCompleted` are published for cycles that (i) associated, (ii) could
  not associate, (iii) failed to resume promiscuous mode
- **Then** the view's status is Working during the drain, then Hunting / Degraded / Fault respectively —
  the same reading the LED surface uses.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario B — the counters accumulate from bus facts (host).**
- **Given** the surface
- **When** successive `DrainCompleted` facts carry upload outcomes and `NewPassword` facts arrive
- **Then** the view's *uploaded* count is the running sum of `accepted + duplicate` and its *cracks*
  count is the number of `NewPassword` facts — no capture-queue depth is shown (the bus carries none).
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario C — the sync line reflects the last sync (host).**
- **Given** the surface
- **When** a `SyncCompleted` fact is published
- **Then** the view records whether the sync was ok and its new-password count, and a later sync
  replaces them.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario D — a new password arms the CRACKED toast, without the PSK (host).**
- **Given** the surface
- **When** a `NewPassword` fact carrying an ESSID, a BSSID, and a PSK is published, then a `tick()` runs
- **Then** the view's toast is active and carries the ESSID and BSSID — and no field exists on the toast
  to carry the PSK, so the recovered password cannot reach the panel.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario E — the toast clears after its hold (host).**
- **Given** an armed toast
- **When** `tick()` runs at a time past the fixed hold
- **Then** the toast is cleared and the view falls back to the HUD (a repeat `NewPassword` re-arms and
  extends the hold).
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario F — non-crack facts never arm the toast (host).**
- **Given** the surface
- **When** `DrainStarted`, `DrainCompleted`, `SyncCompleted`, or `FirstSyncSummary` is published
- **Then** the toast stays inactive (a first-sync backlog raises no banner — ADR-0019 decision #5),
  though the HUD updates.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario G — a liveness heartbeat proves the panel is not frozen (host).**
- **Given** the surface begun at a clock anchor
- **When** `tick()` runs across successive heartbeat half-periods
- **Then** the view's heartbeat flag alternates, so a live HUD is visibly distinguishable from a hung
  board (the reason a status LED blinks — slice-0022).
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario H — the surface renders only when the view changes (host).**
- **Given** the surface driving a fake renderer that counts renders
- **When** `tick()` runs repeatedly with no new fact and no heartbeat-phase change, then a fact changes
  the view
- **Then** an unchanged view renders zero extra times and a changed view renders exactly once — the
  panel sees discrete updates, not a per-tick stream.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario I — a non-printable ESSID byte is made printable before display (host).**
- **Given** the surface
- **When** a `NewPassword` whose ESSID contains a control byte is published
- **Then** the toast's ESSID has the control byte replaced with a printable placeholder, so it cannot
  corrupt the panel text, while a plain-ASCII ESSID is unchanged.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario J — on-air: the HUD and the CRACKED banner render legibly (device).**
- **Given** a flashed board with a panel and a bench probe that injects a synthetic `NewPassword`
- **When** the verify streams the canvas `dump` while the banner is armed
- **Then** the reconstructed PNG shows the status HUD (status word, counters, sync line) and the CRACKED
  banner naming the injected ESSID + BSSID, legibly and correctly oriented, with no `[FATAL]`/`[ERROR]`.
- **Proof:** `npm run verify:screen` (`scripts/0026-screen-verify.mjs`); artifact
  `artifacts/0026-screen-toast.png`.

## Design

Per ADR-0025. Components:

- **`surface/screen_view.h`** (pure) — `enum class ScreenStatus`; `struct ScreenView` (status,
  `uploaded`, `cracks`, last-sync fields, `heartbeat`, and an active-toast flag with `essid[]` + a
  formatted `bssid[]` — **no** password field) plus an equality compare for render-on-change.
- **`surface/screen_renderer.h`** — the abstract `ScreenRenderer` seam (one `render(const ScreenView&)`),
  the surface's device-agnostic output contract.
- **`surface/screen_toast_surface.{h,cpp}`** (pure, host-tested) — `ScreenToastSurface : EventSink`
  holding the model; `begin(now)`, `onAppEvent` (updates the model, arms the toast), `tick(now)` (toast
  expiry + heartbeat + render-on-change). Owns no clock and no hardware.
- **`surface/screen_renderer_esp32.{h,cpp}`** (device-only) — `Esp32ScreenRenderer : ScreenRenderer`
  holding the shared `IDisplay&`; lays out the HUD and the banner and calls `drawText`/`fillScreen`/
  `present`.
- **`hal/display/display_hal.h`** + `lgfx_display.{h,cpp}` + `null_display.h` — `IDisplay` gains
  `drawText(...)`; LGFX forwards to the sprite, `NullDisplay` no-ops (ADR-0002 #4).
- **`hunt_loop.{h,cpp}`** — `huntLoopBegin` gains an optional `IDisplay*`; construct the renderer +
  surface and subscribe + tick them when a display is passed and the board has a panel.
  **`screen_probe.{h,cpp}`** + `main.cpp` dispatch — the bench verify probe (reuses
  `huntLoopInjectCrackedAlert`).

## Verification

- **Scenarios A–I** (pure policy): `test/test_screen_toast_surface/`, native Unity lane (ADR-0004 lane
  1), `npm run test:native`, against a `FakeScreenRenderer` recording the last `ScreenView`.
- **Scenario J** (on-air): `scripts/0026-screen-verify.mjs` (`npm run verify:screen`), driving
  `env:cardputer_testhooks` with `SAPPER_TEST_SCREEN=1`. Reuses slice-0005's canvas-`dump`→PNG path;
  fails on any `[FATAL]`/`[ERROR]`; artifact `artifacts/0026-screen-toast.png` for human review.
- Board compile (ADR-0004 lane 2): `pio run -e cardputer` and `-e cardputer_testhooks`.

## As built

Shipped as designed (ADR-0025), with these deviations and additions worth recording:

- **The serial channel had to be pumped in the probe's loop branch.** The Design listed the probe and
  the verify but missed that every probe's `loop()` branch `return`s before `main.cpp`'s normal
  `g_channel.pump()`. Every prior probe emits its own log lines, but this verify is the first to rely on
  the `dump` command being *answered* while a probe owns the device — so with no pump, `dump` was never
  dispatched and the first on-air run timed out. Fixed by pumping the channel in the screen branch
  (`main.cpp`), reusing the same `g_channel` that reads the same canvas the renderer draws on. This is a
  device-only loop-dispatch change with no host lane; its fail-before/pass-after evidence *is* Scenario J
  (the verify timed out without it, passed with it — see below).
- **`IDisplay` gained one `drawText` primitive** (ADR-0025 decision 3), forwarded to the LGFX sprite's
  `drawString` and no-op'd by `NullDisplay`. The seam stays the single panel owner; the shared
  `display()` singleton is passed into `huntLoopBegin` (a new optional `IDisplay*`, defaulted null, so
  the existing probes and screenless boards are untouched).
- **Measured cost:** shipped `cardputer` image **40.5% → 40.6%** flash (+~0.1%), RAM steady at 35.3% —
  small because LovyanGFX and its base font were already linked for the ADR-0002 bring-up frame; only
  the surface, renderer, and view-model code is new. Both board envs compile.
- **The no-PSK guarantee is structural** (`ScreenView` has no password field, `onAppEvent` copies only
  the ESSID + BSSID), and the ESSID is printable-filtered at enqueue so a control byte in an arbitrary
  802.11 SSID cannot corrupt the panel line. Both are backed by tests the adversarial review confirmed
  *fail* without the code (mutation-verified).
- **Measured host lanes:** screen_toast_surface 10, and the migrated suites unaffected. `pio` native is
  broken in this environment (UnknownPlatform), so the native lane was run via direct g++ compile, the
  established workaround — independently reproduced by the reviewer.
- **On-air Scenario J passed** against the reflashed board: `verify:screen` reconstructed
  `artifacts/0026-screen-toast.png` (240×135, 39.3% of pixels off the dominant colour — the machine
  check that the panel is not blank), showing the HUD (title, status, `up:`/`cracks:` counters, sync
  line) and the `* CRACKED *` banner naming the injected `SAPPER-VERIFY` ESSID + BSSID, legible and
  correctly oriented.
- **The verify probe is network-free**, so the first hourly sync window cannot associate and the HUD
  honestly reads **DEGRADED** in the shot rather than Hunting — which still proves the render path (every
  HUD field + the banner + the amber status colour). The probe's `verifyConfig` widens the drain
  interval to suppress capture/time-triggered windows; it does not (and need not) suppress the sync
  window.
- **Deferrals recorded** (LEDGER): the `statusFromDrain` drain→status mapping is duplicated between the
  LED and the screen (second reader, not third — do not hoist yet); the view-model→renderer split is now
  used by two surfaces (the web dashboard would be the rule-of-three trigger); the HUD shows no
  capture-queue depth (the bus carries no such fact, §4 invariant #13); and a pre-existing display-init
  failure is logged `[FATAL]` but not acted on (the screen surface is now a second consumer of a
  possibly-dead canvas — cosmetic, a headless-first fail-loud fix is out of this slice's scope).
- **Adversarial review (blind, Sonnet 5) — no correctness findings, no rejections.** The reviewer
  compiled the suite and ran two mutation checks (both load-bearing tests bite) and hand-traced the
  vendored LovyanGFX UTF-8 decoder to rule out an over-read on a malformed SSID. Its one out-of-scope
  boyscout observation (the display-init-failure gap above) went to the LEDGER.
