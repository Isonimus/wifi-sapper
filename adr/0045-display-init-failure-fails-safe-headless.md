---
id: '0045'
title: "Display init failure fails safe to headless — a failed panel is treated as an absent panel"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

`setup()` calls `IDisplay::begin()` (real panel init) and logs `[FATAL] display init failed`
when it returns `false` — then **uses the display anyway**: `drawSplashFrame(d)` + `d.present()`
run unconditionally, `runStationBoot` hands `&display()` to `huntLoopBegin` for the status HUD,
and `runMaintenance` draws `drawMaintenanceScreen(display(), …)` + `display().present()`. The
surface bench probes (`screen_probe`, `hunt_hud_probe`, `capture_probe`) are handed `d` the same
way.

This survives today only because LovyanGFX guards a null-buffer sprite: a draw on a panel whose
`begin()` failed is a silent no-op rather than a crash. That is exactly the failure mode the
global quality bar forbids — "no default or fallback that masks a real failure" (§3, fail-loud).
The `[FATAL]` line is printed, but the code then leans on undocumented library leniency to keep
running, so a genuine panel fault is reported *and* papered over in the same breath. It was flagged
by the slice-0026 adversarial pass and carried in `LEDGER.md` as a boyscout debt against §3.

The headless-first architecture already has the right shape for the fix. A screenless board
(`BoardProfile.hasDisplay == false`) resolves the display HAL to `NullDisplay` (ADR-0002 #4), whose
every draw is a no-op and whose `begin()` returns `true` because *absence of a panel is not a
failure*. The whole firmware — capture → upload → sync → alert — runs with no UI dependency
(ADR-0001); the panel is one optional event-bus observer among several. So "the panel is unusable"
is a state the system is already built to run in. A failed `begin()` on a display board should
resolve to that same state, not to a half-drawn dead buffer.

Two shapes of "no panel" already exist and are exercised on every screenless build:

- the reference-taking surfaces (`drawSplashFrame`, `drawMaintenanceScreen`, the surface probes)
  are handed the `NullDisplay` and their draws no-op;
- `huntLoopBegin` is handed `nullptr` and skips wiring the HUD surface entirely, logging
  `[SCREEN] disabled (no display)` (`hunt_loop.cpp` — the `display != nullptr && hasDisplay`
  gate).

Routing a failed panel down these *already-proven* paths introduces no new rendering path — which
matters, because the alternative (wiring `Esp32ScreenRenderer` onto a zero-geometry `NullDisplay`)
would be a rendering configuration nothing else exercises, exactly the untested path §3 rejects.

## Decision

**A real panel init failure is treated as an absent panel: the boot fails loud, then continues
fully headless, and every panel *surface* is routed down the proven screenless path.**

1. **One loud signal, then fail safe.** `setup()` keeps the `[FATAL] display init failed` line
   (extended to say the panel is disabled and the boot continues headless) and captures
   `begin()`'s result in a `panelReady` flag. Nothing downstream reads the raw `display()` for
   *drawing* when `panelReady` is false.

2. **Surfaces that take an `IDisplay&` get the `NullDisplay` on failure.** A single pure policy
   function `panelAfterInit(real, initOk, fallback)` (in `hal/display/display_hal.h`, beside the
   existing pure `resolveDisplay`) returns `real` when `initOk`, else `fallback`. `setup()` binds
   `IDisplay& panel = panelAfterInit(display(), panelReady, nullPanel)` and every reference-taking
   surface draws to `panel`: the boot splash, `runMaintenance`'s `drawMaintenanceScreen`/`present`,
   and the surface probes (`capture_probe`, `hunt_hud_probe`, `screen_probe`). On failure these see
   exactly what a screenless board's `NullDisplay` gives them — the proven no-op path.

3. **The HUD gets `nullptr` on failure.** `runStationBoot` passes `panelReady ? &display() :
   nullptr` to `huntLoopBegin`, reusing the existing "no display" contract so the HUD surface is
   never constructed and `[SCREEN] disabled (no display)` logs — rather than wiring a renderer onto
   a zero-geometry `NullDisplay`, a path nothing else exercises.

4. **Panel-rendering bench probes are skipped on init failure — not exempted, not routed to null.**
   The hooks-only probes whose sole purpose is to render a surface to the panel and dump it —
   `panel_probe` (the ADR-0002 §5 RGB/border/diagonal proof) and the `screen`/`hunt_hud`/`capture`
   surface probes — run only when `panelReady` (a `panelReady` guard around their dispatch in
   `setup()`; inside it `panel` == the real display). A probe cannot fulfil its purpose on a dead
   panel, so continuing is pointless; worse, driving `present()` (a **hardware** push through the
   panel driver, unlike an in-sprite draw) after `begin()` returned `false` is exactly the
   half-draw-on-a-dead-panel this ADR forbids — and for `panel_probe` it would push to an
   uninitialised driver during the very bench session meant to diagnose the fault. On failure the
   `[FATAL]` line is the loud signal and the verify script's missing artifact is the fail; the boot
   then falls through to the non-panel probes and normal boot.

   *Rejected alternative — exempt `panel_probe` and always hand it the real panel:* a first cut did
   this on the reasoning "a raw-panel diagnostic must drive the real panel." It is wrong. A failed
   `begin()` **is** the diagnosis; pushing `present()` to the dead driver diagnoses nothing and risks
   a bus hang. *Rejected alternative — route the surface probes to the `NullDisplay` fallback like the
   shipped surfaces:* they would then claim a false "HUD up", wire a renderer onto a zero-geometry
   `NullDisplay` (the path decision 3 avoids), and dump an empty artifact instead of failing fast.
   Both were caught by this slice's adversarial pass.

5. **The serial `dump` read path is out of scope.** `SerialChannel` binds `display()` by reference
   at static-init (before `begin()` runs) and its `dump` command *reads* the canvas via
   `readCanvas`; it does not draw. On a failed panel `readCanvas` returns 0 bytes (the same guarded
   null buffer), so `dump` is already a harmless no-op — it is the diagnostic read channel, not a
   surface, and rewiring its reference post-init would be invasive for no fail-loud gain. Left as
   is, recorded here so it is not mistaken for an oversight.

The policy in two lines: **a shipped surface treats a failed panel identically to an absent panel**
(draws to the null fallback, or is skipped via `nullptr`, and keeps running); **a bench panel-probe
is skipped entirely** (its purpose requires a live panel). No surface draws on a dead buffer, no new
rendering path is introduced, and the hunt/upload/sync/alert spine runs on exactly as it does on a
screenless board.

## Consequences

- Adds `CLAUDE.md` §4 invariant **#24** (surfaces draw through the init-checked panel selection,
  never the raw `display()`; init failure resolves to the screenless path), in this commit.
- The pure `panelAfterInit` is host-tested in `test_board_profile` (beside `resolveDisplay`) with a
  counting spy display: a draw on the selection lands on the spy when `initOk`, and lands nowhere
  (the `NullDisplay` fallback) when not — a fail-before/pass-after regression against reverting to
  the raw display.
- The wiring (`setup()` threading `panel`/`panelReady`, the `panelReady` guard around the panel
  probes, `runStationBoot`/`runMaintenance` signatures) is device-only and review-only, like every
  boot-path change; `verify:device` (scripts/0005) still dumps the splash from a *working* panel —
  the failure path cannot be forced on real hardware, so it is host-tested (policy) + review-only
  (wiring), not a new device check.
- `runStationBoot` and `runMaintenance` gain a display parameter (`IDisplay*` and `IDisplay&`
  respectively), threaded from `setup()`; both are called once and never return, so the referents
  (function-statics) outlive them.
- Behaviour on a *working* panel is unchanged: `panelReady` is true, `panel` is the real display,
  the HUD gets `&display()`, and every existing frame renders as before.
- Does not supersede ADR-0002 (the capability-based resolution) or ADR-0001 (observers-never-owners);
  it extends them to the runtime-failure case the compile-time capability query could not cover.
