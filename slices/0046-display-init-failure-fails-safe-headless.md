---
id: '0046'
title: "Display init failure fails safe to headless"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0045: on a real panel init failure (`IDisplay::begin() == false`), fail loud once
and then run fully headless — route every panel *surface* down the proven screenless path instead
of drawing on the guarded-but-dead LovyanGFX buffer (§3 fail-loud).

- **Pure policy:** `panelAfterInit(real, initOk, fallback)` in `hal/display/display_hal.h` returns
  the real display when init succeeded, else the `NullDisplay` fallback — host-tested beside
  `resolveDisplay`.
- **Wiring:** `setup()` captures `panelReady`, binds `panel = panelAfterInit(...)`, and threads it
  to the splash, the surface probes, `runMaintenance` (`IDisplay&`), and `runStationBoot`
  (`IDisplay*`, `nullptr` on failure so `huntLoopBegin` reuses the "[SCREEN] disabled" path).
- **Exemption:** `panel_probe` (the raw-panel diagnostic) keeps the real display, cited at the site.

Adds §4 invariant #24. Closes the `LEDGER.md` display-init-fail-loud item (slice-0026 boyscout).

## Definition of Done

**Scenario A — a failed panel routes surface draws to the null display (host).**
- **Given** the pure `panelAfterInit(real, initOk, fallback)` with a counting spy as `real` and a
  `NullDisplay` as `fallback`
- **When** `initOk` is `false` and the returned display is drawn to (`fillScreen`, `drawText`)
- **Then** the spy's draw counter stays at zero (the draw landed on the null fallback), and with
  `initOk` `true` the same draw increments the spy — so a regression to returning the raw display
  fails.
- **Proof:** `test_board_profile` gains `test_panel_after_init_routes_to_fallback_on_failure`
  (fail-before/pass-after).

**Scenario B — a working panel is unaffected (host + device).**
- **Given** `initOk` true
- **When** `panelAfterInit` resolves
- **Then** it returns the real display (same reference), so the splash/HUD/Maintenance render
  exactly as before.
- **Proof:** `test_board_profile` asserts the returned reference is the real display when `initOk`;
  `verify:device` (scripts/0005) still dumps the splash from a working panel (unchanged).

**Scenario C — the boot fails loud, then continues headless (review).**
- **Given** `setup()` with `begin()` returning false
- **When** the boot proceeds
- **Then** `[FATAL] display init failed` is logged (now noting the panel is disabled and the boot
  continues headless), no surface draws to the real panel, and `runStationBoot` passes `nullptr` to
  `huntLoopBegin` so the HUD is skipped and the hunt/upload/sync spine runs on.
- **Proof:** review of the `setup()`/`runStationBoot`/`runMaintenance` wiring (device-only,
  review-only, like every boot-path change).

**Scenario D — panel-rendering bench probes are skipped on init failure (review).**
- **Given** a hooks build (`panel_probe`/`screen`/`hunt_hud`/`capture`) with a failed `begin()`
- **When** `setup()` reaches the probe dispatch
- **Then** the panel probes are skipped (a `panelReady` guard) — none drives `present()` through the
  dead driver and none claims a false success; the `[FATAL]` line and the verify script's missing
  artifact are the loud signal, and boot falls through to the non-panel probes and normal boot.
- **Proof:** review against ADR-0045 decision 4 (device-only wiring, review-only).

## Verification

- **Host (`test_board_profile`, native lane)** — Scenarios A and B: `panelAfterInit` routes draws to
  the fallback on failure and to the real display on success, proven with a counting spy `IDisplay`.
  The display HAL's pure policy already lives here (`resolveDisplay`), so its companion belongs here
  too — no new test dir (§3, cohesion).
- **Device (`verify:device`, scripts/0005, already wired per R11)** — Scenario B's working-panel
  path: a normal build still dumps the splash. Unchanged; the failure path cannot be forced on real
  hardware, so it is host-tested + review-only, not a new device check.
- **Build + review** — Scenarios C and D: the `setup()` fail-loud-then-headless wiring, the threaded
  `panel`/`panelReady`, the `nullptr`-to-`huntLoopBegin` on failure, and the `panel_probe` exemption
  are device-only wiring (review-only, like every boot-path surface change).

## As built

Shipped as designed (ADR-0045). No design changes during implementation.

- **Pure policy (host-tested):** `panelAfterInit(IDisplay& real, bool initOk, IDisplay& fallback)`
  added to `hal/display/display_hal.h` beside `resolveDisplay`. `test_board_profile` gained
  `test_panel_after_init_routes_to_fallback_on_failure` — a `CountingDisplay` spy proves a draw
  lands on the spy when `initOk` and lands nowhere (the `NullDisplay` fallback) when not.
- **Wiring (review-only + verify:device):** `setup()` captures `const bool panelReady = d.begin()`,
  logs the extended `[FATAL]` on failure, and binds `IDisplay& panel = panelAfterInit(d, panelReady,
  nullPanel)` (a function-local `static NullDisplay nullPanel`). The shipped surfaces draw to `panel`:
  the boot splash, and `runMaintenance(creds, panel)`'s Maintenance screen — a null no-op on failure,
  so the SoftAP still serves. `runStationBoot(creds, panelReady ? &display() : nullptr)` passes the
  pointer to `huntLoopBegin`, so a failed panel reuses the proven `[SCREEN] disabled` path.
- **Panel probes skipped on failure:** the four panel-rendering bench probes (`panel_probe`,
  `capture`, `hunt_hud`, `screen`) dispatch inside `if (panelReady)`; a dead panel cannot be
  rendered-and-dumped, and `panel_probe`'s `present()` would otherwise drive an uninitialised driver
  (adversarial-pass finding). Inside the guard `panel` == the real `d`. No exemption remains.
- **Out of scope, recorded:** the serial `dump` read path keeps its static-init `display()`
  reference (it reads, does not draw; a no-op on a failed panel), per ADR-0045 decision 5.
- **Docs:** `CLAUDE.md` §4 gained invariant #24; `LEDGER.md` display-init item deleted.
- **Host:** all 27 native suites pass, warning-clean under `-Wall -Wextra` (`test_board_profile`
  gained the `panelAfterInit` test; no new suite).
- **Boards:** both build clean under `-Wall -Wextra` — `cardputer_testhooks` and `cardputer`.
- **Adversarial pass (Sonnet 5, blind to intent, aware of law):** two real findings, both accepted
  and fixed by the same simplification. (1) The first-draft `panel_probe` exemption drove `present()`
  — a hardware push — through an uninitialised driver on a failed `begin()`, the exact half-draw #24
  forbids. (2) Routing the surface probes to the null fallback made them claim a false "HUD up", wire
  a renderer onto a zero-geometry `NullDisplay`, and dump an empty artifact. Fix: the four
  panel-rendering probes now dispatch inside `if (panelReady)` and are skipped on failure — shipped
  surfaces degrade to null and keep running, bench probes are skipped (their purpose requires a live
  panel). The exemption was removed. A third nit (stale "display is live" comments in the surface
  probes) dissolved: with the guard those probes run only when the panel is live. Static-init order,
  `nullPanel` lifetime, `panelAfterInit`/its test, and `NullDisplay` zero-geometry safety were checked
  and ruled clean.
