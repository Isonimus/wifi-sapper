---
id: '0034'
title: "Live hunt HUD: the panel shows the network being sniffed with B/M1-M4 markers and a progress bar"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Show, on the panel, what the appliance is doing *right now*: the network currently being sniffed and
how much of its handshake has been collected. Per ADR-0033: add a read-only `HuntSnapshot` pull seam on
`HuntEngine` (phase, channel, discovered count, target bssid/ssid, Beacon/M1–M4 flags); have
`ScreenToastSurface` pull it each `tick()` and render a live-hunt line — a SCAN line while Discovering,
and the target + a B/M1/M2/M3/M4 indicator row + a progress bar while Capturing. Live state flows only
through the pull snapshot, never the event bus (§4 #13/#18). A `SAPPER_TEST_HOOKS` `huntLoopInjectFrame`
(through the real `onFrame` seam, §4 #2) lets the on-air verify light the indicators deterministically.

## Definition of Done

**Scenario A — the snapshot reflects Discovering (host).**
- **Given** a `HuntEngine` over a `FakeRadioSniffer`, begun, in Discovering after a beacon is ingested
- **When** `huntSnapshot()` is read
- **Then** `phase == Discovering`, `channel` is the parked channel, and `discovered` equals the AP count.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario B — the snapshot reflects a populated in-flight capture (host).**
- **Given** the engine advanced into Capturing a target, with a beacon + M1 + M2 for that target ingested
- **When** `huntSnapshot()` is read
- **Then** `phase == Capturing`, `bssid`/`ssid` equal the target's, and `hasBeacon`/`hasM1`/`hasM2` are
  true while `hasM3`/`hasM4` are false — the live message set the collector holds.
- **Proof:** `npm run test:native` → `test/test_hunt_engine`.

**Scenario C — the surface renders a SCAN line while Discovering (host).**
- **Given** a `ScreenToastSurface` with a fake renderer and a fake `HuntSnapshotSource` reporting
  Discovering on channel 6 with 3 seen
- **When** `tick()` runs
- **Then** the `ScreenView` hunt fields show `phase == Discovering`, `channel == 6`, `discovered == 3`
  (the renderer draws the SCAN line); no indicator row.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario D — the surface renders the target + indicators + progress while Capturing (host).**
- **Given** the fake source reports Capturing "lab-ap"/a BSSID with Beacon+M1+M2 present
- **When** `tick()` runs
- **Then** the `ScreenView` shows the target SSID (printable-filtered; BSSID when the SSID is empty) and
  the B/M1/M2 flags set with M3/M4 clear, and a progress value of 3-of-5.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario E — no source wired renders the pre-0033 HUD (host).**
- **Given** a `ScreenToastSurface` with NO hunt source set
- **When** `tick()` runs
- **Then** the `ScreenView` has no live-hunt content (the counters/toast HUD is unchanged), and
  render-on-change still holds — `operator==` accounts for the new hunt fields, so a static view does
  not redraw.
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario J — the live HUD lights up on real hardware (on-air, deterministic).**
- **Given** a `cardputer_testhooks` board running the hunt-HUD probe (`SAPPER_TEST_HUNT_HUD=1`)
- **When** the probe defers the cracked-sync (so no STA window stops the engine) and injects a beacon +
  M1 — a *partial* handshake, so the engine stays Capturing rather than reporting and advancing —
  through the real `onFrame` seam (§4 #2), and the verify sends `dump`
- **Then** the device logs a `[HUNT-HUD] populated` readiness line (no `[FATAL]`/`[ERROR]`) and the panel
  dump is a non-blank frame showing the target and the Beacon + M1 indicators lit (progress 2/5) — the
  live HUD, on hardware.
- **Proof:** `npm run verify:hunt-hud` → `scripts/0034-hunt-hud-verify.mjs` (asserts readiness + a
  non-blank panel; writes `artifacts/0034-hunt-hud.png` as the eyeball artifact).

## Design

- **Seam (`net/hunt_snapshot.h`, new)**: `struct HuntSnapshot { Phase phase; uint8_t channel; size_t
  discovered; uint8_t bssid[6]; char ssid[33]; bool hasBeacon, hasM1, hasM2, hasM3, hasM4; }` and
  `class HuntSnapshotSource { virtual HuntSnapshot huntSnapshot() const = 0; }`. `Phase` is reused from
  `HuntEngine` (moved/aliased so the POD header does not pull the engine).
- **Engine**: `HuntEngine` implements `HuntSnapshotSource`; `huntSnapshot()` reads `phase_`,
  `parkedChannel_`, `registry_.count()`, and `collector_.handshake()` (bssid/ssid/`has*`). A comment at
  the site cites ADR-0033 decision 3: the read is intentionally lock-free/cosmetic (driver-task writes,
  app-task read; flags monotonic within a capture) — do not "fix" it with a lock (§4 #10).
- **View (`surface/screen_view.h`)**: add a hunt section (phase, channel, discovered, target ssid/bssid,
  the five flags) to `ScreenView`, compared in `operator==`.
- **Surface (`screen_toast_surface.*`)**: add `setHuntSource(HuntSnapshotSource&)`; in `tick()`, if a
  source is set, pull the snapshot and populate the view's hunt fields (printable-filtering the SSID).
- **Renderer (`screen_renderer_esp32.cpp`)**: draw the SCAN line (Discovering) or the target + indicator
  row + progress bar (Capturing), mirroring `../adversary` `handshake_screen.cpp`'s `drawIndicator`.
- **Wiring (`hunt_loop.cpp`)**: after the engine and screen exist, `g_screen->setHuntSource(*g_engine)`.
- **Test stimulus (`hunt_loop.cpp`, `SAPPER_TEST_HOOKS`)**: `huntLoopInjectHudStimulus()` builds a
  synthetic beacon + M1 — a *partial* handshake (reusing the shared `buildStimulusBeacon` + `appendEapol`)
  — and feeds each through `g_engine->onFrame` (the real seam, §4 #2), absent from shipped builds (§4 #4).
  A complete set would make the engine report-and-advance, so the HUD would not persist; partial keeps it
  Capturing. `huntLoopDeferSync()` (also test-hooks) pushes the cracked-sync deadline out so no STA
  window stops the engine on a network-less bench. A new `hunt_hud_probe` calls both each cycle so the
  engine discovers → captures → populates its collector, renders the panel, and answers `dump`.

## Verification

- **Host (`npm run test:native`)** proves the logic — Scenarios A–E:
  - `test/test_hunt_engine` — `huntSnapshot()` reflects Discovering and a populated Capturing (A, B).
  - `test/test_screen_toast_surface` — SCAN line, target + indicators + progress, no-source fallback,
    render-on-change with the new fields (C, D, E), against a fake `HuntSnapshotSource` and fake renderer.
  Built with `g++ -Wall -Wextra`; a warning is a failure.
- **On-air (`npm run verify:hunt-hud` → `scripts/0034-hunt-hud-verify.mjs`)** proves Scenario J: drives
  the hunt-HUD probe, asserts the `[HUNT-HUD]` readiness line and a non-blank panel dump, and writes
  `artifacts/0034-hunt-hud.png` for the eyeball review of the lit indicators. Attached board, never
  cloud CI (§4 #5).

## As built

Shipped as designed. `net/hunt_snapshot.h` holds `HuntPhase` (aliased as `HuntEngine::Phase`, so existing
`HuntEngine::Phase::X` still names it), the `HuntSnapshot` POD, and the `HuntSnapshotSource` seam;
`HuntEngine` implements `huntSnapshot()` as a lock-free, best-effort app-task read (§4 #18).
`ScreenToastSurface::setHuntSource()` pulls it each tick into new `ScreenView` hunt fields (all in
`operator==`); `Esp32ScreenRenderer` draws a SCAN line while Discovering, the target + a B/M1/M2/M3/M4
indicator row + a progress bar while Capturing, and an idle line otherwise. Live state never touches the
bus (no new `AppEventType`). The wiring is `g_screen->setHuntSource(*g_engine)` in `hunt_loop`.

**Two deviations from the plan, both forced by the on-air run (the verify earned its keep):**
1. The first `verify:hunt-hud` failed — the cracked-sync scheduler is due at boot and stays due while it
   cannot associate, so it opened STA windows that `stop()` the engine (a WORKING↔DEGRADED flip) and the
   hunt never sustained Capturing. Added a `SAPPER_TEST_HOOKS` `huntLoopDeferSync()` (pushes the sync
   deadline out via the existing `noteSynced`); the probe calls it before the first pump.
2. The stimulus injects a **partial** handshake (beacon + M1, no M2), not a complete one: a wpa-sec-valid
   set makes the engine report-and-advance at once, so the populated HUD would not persist for the
   `dump`. Beacon + M1 keeps the engine Capturing with a stable, dumpable HUD (B + M1 lit, 2/5 progress).

Proven: host **25 dirs green** — `test_hunt_engine` 16 (incl. snapshot reflects Discovering / populated
Capturing / hides a stale target when not Capturing), `test_screen_toast_surface` 17 (SCAN line, target +
indicators + progress, printable-filtered SSID, idle line, no-source fallback + render-on-change),
`test_handshake_collector` 7 (unchanged after the single-shot SSID write) — `g++ -Wall -Wextra`, no
warnings (`pio` native broken here). Both boards compile clean: shipped `cardputer` Flash **40.7%
(1,361,343 bytes)**; `cardputer_testhooks` builds the probe (1,361,959). On-air **Scenario J PASS** on a
real Cardputer ADV via `verify:hunt-hud`: `[HUNT-HUD] populated ssid='SAPPER' B=1 M1=1 M2=0 M3=0 M4=0`
and a rendered panel (`artifacts/0034-hunt-hud.png`, 5.9% of pixels off the dominant colour). No PSK or
pcap byte on any surface, serial line, or the artifact.

A blind adversarial pass (Sonnet 5) found no crash, OOB, secret leak, or invariant violation. Its
findings were addressed: (1) a confirmed `ssid` data race — `beaconSsid`'s in-place clear-then-fill on the
driver task vs the app-task snapshot read could momentarily read an empty SSID — fixed by publishing
`handshake_.ssid` in a single memcpy of a scratch-built complete value (no empty window), with the ADR/#18
wording corrected to scope the no-lock justification per field; (2) a coverage gap for the non-Capturing
snapshot path — added the stale-target-hidden engine test and the idle-line surface test; (3) a "single
byte" wording nit — corrected. The pre-existing `deauth_probe`/`rf_sniff_probe` warnings surfaced by the
board build were logged as a LEDGER defect (boyscout), not fixed in this slice.
