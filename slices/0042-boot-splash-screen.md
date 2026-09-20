---
id: '0042'
title: "Boot splash: a product face at power-on; the panel proof moves to a bench probe"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0041. Replace the shipped boot face — the RGB-blocks + border + diagonal **panel
diagnostic** (`drawBringupFrame`) — with a real **product splash** (`drawSplashFrame`): the
name `"WiFi Sapper"`, the firmware version (`kFirmwareVersion`), and a one-line tagline, drawn
with the `drawText` primitive slice-0026 already proved on this panel. Relocate the diagnostic,
intact, behind a `SAPPER_TEST_PANEL` bench probe (`panel_probe.{h,cpp}`) so no shipped build can
render it at boot, while `verify:device` still dumps it from a hooks build — the exact colour-order
/ offset / mirror / rotation proof slice-0005 asserted (ADR-0002 §5), only off the shipped face.

## Definition of Done

**Scenario A — the shipped boot face is the product splash (device).**
- **Given** a normal `cardputer` build (no test hooks) flashed to the board
- **When** it boots and `verify:device` requests a canvas `dump`
- **Then** no `[FATAL]` line appears and the reconstructed PNG shows `"WiFi Sapper"`, the
  firmware version, and the tagline rendered within the 240×135 panel (no clipping) — and **not**
  the RGB-blocks / border / diagonal test pattern.
- **Proof:** `npm run verify:device` against a normal build → `artifacts/0005-cardputer-bringup.png`
  (human-reviewed splash) + the machine half (no `[FATAL]`, geometry present).

**Scenario B — the panel proof is still reachable, unchanged, behind the probe (device).**
- **Given** a `cardputer_testhooks` build with `SAPPER_TEST_PANEL=1` flashed to the board
- **When** it boots (the panel probe takes the device) and `verify:device` requests a `dump`
- **Then** no `[FATAL]` line appears and the reconstructed PNG shows the three RGB565 primary
  blocks in order, the one-pixel white border on all four edges, and the corner-to-corner
  diagonal — the exact ADR-0002 §5 proof slice-0005 confirmed, byte-for-byte the same frame,
  now sourced from `panel_probe` rather than the shipped boot path.
- **Proof:** `npm run verify:device` against the `SAPPER_TEST_PANEL` build → the same script,
  same PNG reconstruction, run against the probe build.

**Scenario C — no shipped build can render the diagnostic (build + review).**
- **Given** a shipped (non-hooks) build, where `SAPPER_TEST_PANEL` is undefined
- **When** it compiles and `setup()` runs
- **Then** `panel_probe` and `drawBringupFrame` are compiled out (behind `#ifdef SAPPER_TEST_HOOKS`
  like every other probe), and the boot path calls `drawSplashFrame` only — so the test pattern
  is structurally absent from a shipped unit's boot face (§4 #22, aligning §4 #4).
- **Proof:** `npm run build:boards` (the shipped `cardputer` env links with the probe absent) +
  review of the `setup()` dispatch; corroborated by Scenario A's normal-build dump showing the
  splash, not the pattern.

## Verification

- **Device (`npm run verify:device`, already wired in `package.json` per R11)** — Scenarios A and B:
  `scripts/0005-cardputer-bringup-verify.mjs` reconstructs the boot canvas into a PNG for review and
  fails loud on any `[FATAL]`/`[ERROR]`, a malformed dump, or a non-read-only observation channel.
  The same wired script proves both faces by which build is flashed: a **normal** build dumps the
  splash (A); a **`SAPPER_TEST_PANEL`** build dumps the panel proof (B). No new verify script is added
  — the splash is static content drawn with a primitive `verify:screen` (slice-0026) already proved on
  this panel and has no data-dependent logic, so a dedicated splash-verify would run once and be dead
  thereafter (§3); reusing the one script also avoids copying its serial+PNG machinery a third time
  (it already lives in slice-0005 and slice-0026 — the rule of three).
- **Build + review** — Scenario C: `npm run build:boards` links the shipped `cardputer` env with the
  probe compiled out; the `setup()` probe dispatch and the boot-face call are device-only wiring
  (review-only, like the other probe dispatches). There is no new host-testable logic in this slice:
  the splash and the relocated proof are both rendering concerns (device-only, PNG-verified), following
  the screen-renderer precedent that only pure view-models are host-tested — and a static splash has
  no view-model.

## As built

Shipped as designed (ADR-0041), no design changes during implementation — a clean relocation, not a
logic change.

- **Splash (shipped face):** `drawSplashFrame(IDisplay&)` in `main.cpp` replaces the `drawBringupFrame`
  call in `setup()` — `"WiFi Sapper"` (green, size 3) + `v<kFirmwareVersion>` + `autonomous handshake
  hunter`, via `drawText`. New `src/config/firmware_version.h` holds the single `kFirmwareVersion`
  (`"0.1.0"`).
- **Panel proof (relocated, intact):** the RGB-blocks/border/diagonal frame, its `fillRect` helper, and
  the diagnostic RGB565 primaries moved **verbatim** from `main.cpp` into new device-only
  `src/panel_probe.{h,cpp}`, gated on `SAPPER_TEST_HOOKS` with a `SAPPER_TEST_PANEL` runtime check —
  mirroring `screen_probe` exactly. `setup()` dispatches `panelProbeBegin(d)` first among the probes;
  `loop()` gained a `panelProbeActive()` branch that pumps the serial channel so `dump` is answered
  while the device parks on the frame. `platformio.ini`'s `cardputer_testhooks` env gained
  `-DSAPPER_TEST_PANEL`.
- **Verify:** `scripts/0005-cardputer-bringup-verify.mjs` (`verify:device`) is unchanged in its
  assertions; its header now documents that a normal build dumps the splash and a `SAPPER_TEST_PANEL`
  build dumps the panel proof. `main.cpp` no longer references `drawBringupFrame`, `fillRect`, `kRed`,
  or `kBlue` (`kBlack`/`kWhite`/`kGreen` remain, used by the splash and the Maintenance panel).
- **No new host test, by design:** the splash and the relocated proof are both device-only *rendering*
  (no view-model logic), so — per the screen-renderer precedent (only pure view-models are host-tested)
  and §3's caution against a run-once verify — the proof is the `verify:device` PNG against both builds,
  not a host suite. `drawText` on this panel was already device-proven by `verify:screen` (slice-0026).
- **Host:** all 27 native suites still pass, warning-clean under `-Wall -Wextra` (no host-compiled file
  changed; the relocation is device-only).
- **Boards:** both build clean under `-Wall -Wextra` — `cardputer` Flash 41.0% (1,368,935 bytes),
  `cardputer_testhooks` 41.0% (1,369,595 bytes), RAM 35.6%.
- **Adversarial pass (Sonnet 5, blind to intent, aware of law):** no finding meeting the bar — buffer
  safety, splash text geometry within 240×135, the compile-gating that keeps the diagnostic out of
  shipped builds, probe dispatch/lifecycle, and `kFirmwareVersion` linkage all checked. One dismissed
  point (a `span - 1` divisor that would divide by zero only on an impossible 1-pixel panel, in the
  verbatim-moved frame) was pinned with a site comment so it is not re-litigated (stele:ADR-0012).
