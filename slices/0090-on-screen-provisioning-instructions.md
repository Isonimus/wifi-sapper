---
id: '0090'
title: "First-boot on-screen provisioning instructions"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Show the first-boot provisioning instructions on the panel, so an operator who flashed from m5burner
(and so never saw the README) can provision a screen-equipped device without reading a serial log. Today
`startPortal()` brings up the SoftAP but draws nothing: the boot splash persists, and the join details
(`ssid`/`pass`/`url`) go to serial only. This slice draws a guided setup screen during the Provisioning
phase, implementing ADR-0089 (which refines §4 #22's "boot face renders only the splash").

This is item 2 of the HUD/UI feature set (after slice-0088's resolution-aware HUD + version, item 1); the
battery indicator (item 4) is the next and last slice. It does not close the slice-8 hardware-broadening
umbrella.

## Definition of Done

- **Given** an unprovisioned board with a display (Cardputer, Stick, T-embed, CYD)
- **When** it boots into the Provisioning phase and the SoftAP comes up
- **Then** the panel shows the setup instructions: the SoftAP name (`Sapper-XXXX`), the join password
  (`sapper-setup`), and the setup URL (`http://192.168.4.1/`), in guided numbered steps, and no field is
  clipped off the 240x135 panel.

- **Given** a board whose panel failed to initialise, or a screenless board
- **When** it enters the Provisioning phase
- **Then** the SoftAP still comes up and the serial `[PORTAL]` banner still logs, and the on-screen draw
  is a safe no-op (the `NullDisplay` fallback, ADR-0045): no `[FATAL]`, no half-draw.

- **Given** any board entering the Provisioning phase
- **When** the portal starts
- **Then** the serial `[PORTAL] ssid=... pass=sapper-setup url=http://...` banner is unchanged (the
  headless provisioning path is untouched), and `[STATE] provision` is still reported.

## Design

**Existing code considered.** `drawMaintenanceScreen` (`main.cpp`) is the pattern reused: `fillScreen` +
a few `drawText` lines at fixed coordinates, drawn once, no-op on a `NullDisplay`. `drawSplashFrame`
(`main.cpp`, ADR-0041) is the sibling it sits beside. Both are fixed-coordinate and top-left; the new
screen matches them deliberately (ADR-0089 decision 3), not slice-0088's resolution-aware `Esp32ScreenRenderer`
(that is a live-HUD device renderer on the `ScreenRenderer` seam, a different surface; porting its two-tier
layout to the three boot-face screens is a separate deferred slice, LEDGER). The join values come from
`g_portal.apSsid()`, `kSoftApPassword`, and the AP IP, the **same** sources the serial `[PORTAL]` banner
already uses, so no new data source and no magic version/URL string (DRY).

**Draw site.** `startPortal()` gains an `IDisplay& panel` parameter (the init-checked `panel` from
`setup()`, which is the `NullDisplay` fallback on a failed panel, ADR-0045) and draws the screen once after
`g_portal.begin()` succeeds, when `apSsid()` and `softAPIP()` are valid. `startPortal()` is called from two
sites (`setup()` first-boot, and the STA-fail fallback in `runStationBoot`); both pass their panel. The
content is static during provisioning, so one draw (like Maintenance), no per-loop redraw.

**Content (guided, newbie-first).** A `SAPPER SETUP` heading (size 2), then: `1. Join WiFi:` with the SSID and
`pass: sapper-setup` on following lines, and `2. Open in browser:` with the URL. Tuned to fit 240x135 at
`drawText` size 1 (40 cols): `Sapper-XXXX` and `http://192.168.4.1/` are each < 40 chars.

**Security.** The password shown is the fixed **public default** (`kSoftApPassword`), not a per-device
secret, so rendering it on the shoulder-surfable panel leaks nothing (ADR-0089 Context). This is the
deliberate opposite of §4 #21's per-device Maintenance PSKs, which stay off the panel. A code comment at the
draw site records this so nobody "fixes" it by hiding the password.

## Verification

This is a **device render** on the boot face: its correctness is the on-air photograph, not a unit test,
exactly like `drawSplashFrame`/`drawMaintenanceScreen` (ADR-0041; the native lane has no view-model to
assert here). Proofs:

- **Error gate (machine-checked, CI-wired):** `scripts/0007-provisioning-verify.mjs` drives an unprovisioned
  boot, asserts `[STATE] provision` and the `[PORTAL]` banner and no `[FATAL]` (DoD scenarios 2 and 3's
  log/boot half). Unchanged and already wired in `package.json` (§4 #3 / ADR-0006).
- **Build check (machine-checked):** the `cardputer`, `m5stickc_plus2`, `t_embed`, and `cyd_3248s035c` envs
  compile and link (the new parameter and draw call touch `main.cpp`, shared by all four).
- **Native suite (machine-checked):** no seam or view-model change, so `npm run test:native` passes
  untouched (proof the slice added no host-side logic).
- **Photographic (eyes-on, operator-run):** a setup-screen photo on a 240x135 board (DoD scenario 1). Per
  the photographic-proof doctrine for boot-face renders (§4 #22, ADR-0041). If not taken this session it is
  tracked in LEDGER like slice-0085's/0088's deferred boot-face photos.

## As built

Shipped as designed and confirmed on device (M5StickC Plus2).

- `drawProvisioningScreen(IDisplay&, ssid, password, url)` was added beside `drawMaintenanceScreen` in
  `main.cpp`: a `fillScreen` + fixed-coordinate `drawText` lines (a `SAPPER SETUP` heading, then `1. Join WiFi:`
  with the SSID and `pass: <password>`, then `2. Open in browser:` with the URL), matching the
  splash/Maintenance sibling style (not resolution-aware, ADR-0089 decision 3). The values come from
  `g_portal.apSsid()`, `kSoftApPassword`, and a `url` string built once from `WiFi.softAPIP()` and reused
  by the serial `[PORTAL]` banner (DRY, no new data source).
- `startPortal()` became `startPortal(IDisplay* panel)` (nullable, matching `runStationBoot`'s existing
  HUD-handle convention): after `g_portal.begin()` succeeds it logs the unchanged banner, then, when the
  panel is non-null, draws the screen once and `present()`s it. `nullptr` (a failed or absent panel,
  ADR-0045) skips the draw, the proven headless path. Both call sites pass their panel: `setup()` passes
  `panelReady ? &display() : nullptr`, the STA-fail fallback in `runStationBoot` passes its `panel`.
- A code comment at `drawProvisioningScreen` records the shoulder-surf exception (the shown password is the
  public default `kSoftApPassword`, not a per-device secret, the opposite of §4 #21) so it is not "fixed"
  by hiding it.
- Machine checks (2026-10-28): lint 0/0, index regenerated, native suite 340/340 (no seam/view-model
  change). All four display envs built SUCCESS with no `.dram0.bss` overflow: cardputer RAM 46.4% / Flash
  1382919 B, m5stickc_plus2 RAM 29.3% / Flash 1404843 B, t_embed RAM 28.9% / Flash 1385339 B,
  cyd_3248s035c RAM 29.2% (95752 B, unchanged) / Flash 1392195 B (+320 B vs slice-0088 for the new screen).
- **DoD scenario 3 met (machine-checked):** the serial banner is unchanged in output (`url=http://...` now
  built from a reused `url` string; the emitted text is identical), and `[STATE] provision` still reports.
- **DoD scenarios 1 and 2 met on device (2026-10-28):** the M5StickC Plus2 was erased and flashed with the
  normal (unprovisioned) build; it booted into the Provisioning phase (`[STATE] phase=provisioning`,
  `disp=240x135`, no `[FATAL]`), raised `Sapper-DE49` with the `[PORTAL]` banner, and `verify:provisioning`
  scenario C PASSED (portal up while serial still answers `ping`/`state`, §4 #3). The operator confirmed the
  panel shows the `SAPPER SETUP` screen with the SSID, `sapper-setup`, and `http://192.168.4.1/`, no field
  clipped. No deferred photo audit remains.
