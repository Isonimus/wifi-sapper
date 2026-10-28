---
id: '0094'
title: "Inactivity screen dimming (two-stage backlight)"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Dim the panel backlight on a hunting Sapper after a configurable idle, in two stages (readable dim,
then off), to extend battery runtime and reduce the OPSEC tell of a lit screen on a planted unit,
without ever pausing the hunt. Implements ADR-0093. The backlight is the largest avoidable draw, and
the device hunts unattended, so a dark-when-idle screen that brightens on a capture/crack is the right
behaviour for this headless-first appliance.

Picks up the LEDGER feature item for auto screen dimming (adapted from `adversary:ADR-0116`). It does
not close the slice-8 hardware-broadening umbrella.

## Definition of Done

- **Given** a board with a panel, provisioned with `screenDimAfterSec`=D and `screenOffAfterSec`=O (O>=D>0)
- **When** it hunts and no capture/crack/input occurs
- **Then** the backlight is full until D seconds idle, drops to the readable dim level between D and O,
  and goes fully off after O seconds; a `[DIM] stage=...` line logs each transition, and the hunt loop
  keeps running (capture/upload/sync continue) through the dimmed and off states.

- **Given** a dimmed or off screen on a hunting board
- **When** a `HandshakeCaptured` or `NewPassword` fact arrives (or, on a board with a pollable input, any
  input)
- **Then** the backlight returns to full and the idle timer restarts; a routine `SyncCompleted` does not
  wake it.

- **Given** a board provisioned with `screenDimAfterSec`=0 (the default), or a screenless board
- **When** it hunts
- **Then** the backlight never dims (feature off), exactly as before this slice, and no `esp_light_sleep`
  / `esp_deep_sleep` is ever called.

## Design

**Existing code considered.** The pure-predicate + device-wiring split and the two-stage shape are taken
from `adversary:ADR-0116` (`inactivityStage`), adapted: activity is a bus event here, not user input.
The controller is a bus observer like `LedStatusSurface`/`ScreenToastSurface` (ADR-0021/ADR-0025):
`onAppEvent` notes activity without a clock, `tick(now)` makes the clock-based stage decision and is the
only thing that touches the backlight, exactly the surface idiom already in the tree. The backlight is
driven through the `IDisplay` seam (ADR-0002), not a raw GPIO, so a null/absent panel dims nothing. The
config rides the existing provisioning seam (ADR-0006) and setup form (ADR-0037), reusing the
`resolveProvisioningUpdate` pair-from-submission merge, not a new NVS site.

**Seam.** `IDisplay` gains `setBrightness(uint8_t level)` (0-255). `LgfxDisplay`/`DirectPanelDisplay`
forward to `m_panel.setBrightness(level)` (LovyanGFX backlight); `NullDisplay` no-ops it.

**Pure predicate.** `enum class ScreenSleepStage { Active, Dimmed, Off };` and
`ScreenSleepStage inactivityStage(uint32_t nowMs, uint32_t lastActivityMs, uint32_t dimAfterMs,
uint32_t offAfterMs)`: `dimAfterMs==0` -> Active (disabled); elapsed `= nowMs - lastActivityMs`
(wrap-safe); `offAfterMs!=0 && elapsed>=offAfterMs` -> Off; `elapsed>=dimAfterMs` -> Dimmed; else
Active. Robust to `offAfterMs < dimAfterMs` (Off still wins past its threshold).

**Controller.** `ScreenSleepController` (in `surface/`) holds `IDisplay&` + the two delays (ms).
`begin(now)`: full brightness, seed `lastActivity_=now`. `onAppEvent`: set a pending-activity flag on
`HandshakeCaptured`/`NewPassword` (no clock). `noteInput(now)`: reset activity (called from `loop()` on
a board input). `tick(now)`: fold the pending flag into `lastActivity_`, compute the stage, and on a
stage change call `setBrightness` (full / `kScreenDimBrightness` ~38 / 0) and log `[DIM] stage=...`.
Wired in `hunt_loop` beside the screen surface, only when a display is present and `dimAfterSec>0`.

**Config.** `ProvisioningRecord.screenDimAfterSec` / `screenOffAfterSec` (uint16, default 0). Two number
inputs on the setup form, **prefilled** with the stored value (`value='N'`, carried on a non-secret
`SetupFormModel` field) so a re-save to change something else preserves them, like the checkboxes preserve
their state (they are config, not secrets, so echoing the value is safe). `provisioning_store` round-trips
them (absent -> 0). The form field is parsed server-side by the pure `parseDelaySeconds` (clamped to the
u16 range, since the HTML `max` is advisory and a raw POST could over/underflow a cast), and
`resolveProvisioningUpdate` clamps `screenOffAfterSec` (if nonzero) to `>= screenDimAfterSec` on both the
first-boot and re-save paths.

**Input wake.** `loop()` (Ready phase) calls `huntLoopNoteInput(now)` when the board reports an input:
keyboard boards on any key, button boards on the entry button reading pressed (a press that is also the
start of a Maintenance hold still wakes, which is fine). A screenless/input-less board never calls it.

## Verification

- **`test/test_screen_sleep` (native, CI):** `inactivityStage` at each boundary (Active/Dimmed/Off, the
  D and O edges, `dimAfter==0` disabled, `offAfter==0` dim-only, wrap-safety, out-of-order delays) and
  `ScreenSleepController` against a fake `IDisplay` + fake clock (a capture/crack wakes, a SyncCompleted
  does not, a stage change calls `setBrightness` once, `noteInput` wakes). Proves DoD scenarios 1-3's
  logic.
- **`test/test_provisioning_store` + `test/test_provisioning_form`:** the two new fields round-trip
  (absent -> 0); the form prefills them from the model (so a re-save preserves them) while no secret field
  is prefilled; `parseDelaySeconds` clamps an out-of-range/negative POST to the u16 range; and the off>=dim
  clamp holds.
- **Build check (native, CI + machine):** all four display envs compile and link (seam + controller +
  record + form touch shared code).
- **`scripts/0094-dim-verify.mjs` (`verify:dim`, hardware lane):** provisions short delays, drives the
  hunting Stick, asserts the `[DIM] stage=dimmed` then `stage=off` lines appear with no `[FATAL]` and the
  hunt keeps logging (loop never paused), and that a capture injection brings `stage=active` back. Wired
  in `package.json` (§4 #3 / R11). The physical dimming is the photographic half.

## As built

Shipped as designed and confirmed on device (M5StickC Plus2).

- The seam/predicate/controller landed as planned: `IDisplay` gained `setBrightness(uint8_t)` (forwarded to
  the LovyanGFX panel in `LgfxDisplay`/`DirectPanelDisplay`, no-op in `NullDisplay`); the pure
  `inactivityStage` predicate and the `ScreenSleepController` (a bus observer driving the backlight) live in
  `surface/screen_sleep.*` and are host-tested. `hunt_loop` wires the controller only when a display is
  present and `screenDimAfterSec > 0`, logs `[DIM] stage=...` on each transition, and `main.cpp` wakes it on
  a button press (`huntLoopNoteInput`). The `[DIM]` line is edge-triggered (no heartbeat needed, unlike
  `[BATT]`): the stage changes on its own after idle, so a transition is always observable.
- The backlight-only invariant (§4 #41) holds: the dim path touches only `setBrightness`; no
  `esp_light_sleep`/`esp_deep_sleep` anywhere, confirmed by the adversarial review and by the on-air proof
  that the hunt kept logging through the off stage.
- The adversarial correctness review (Sonnet, blind-to-intent) found two config-plumbing defects, both
  fixed here: (1) the dim/off delays were not prefilled, so an unrelated re-save silently reset them to 0;
  they now render their stored value (`SetupFormModel` + `value='N'`), preserved across a re-save like the
  checkboxes, with `test_form_prefills_the_screen_dim_delays_from_the_model`; (2) `handleSave` used an
  unbounded `atoi` that wrapped an out-of-range POST (`70000 -> 4464`, `-1 -> 65535`); a pure
  `parseDelaySeconds` now clamps to the u16 range server-side, with `test_parse_delay_seconds_clamps_out_of_range`.
  The review confirmed the predicate, controller, seconds->millis conversion, seam, and the backlight-only
  invariant correct.
- Config: `ProvisioningRecord` gained `screenDimAfterSec`/`screenOffAfterSec` (NVS-backed, absent -> 0 = off);
  the native mock `Preferences` grew `getUShort`/`putUShort`; `resolveProvisioningUpdate` clamps the off
  delay `>= ` the dim delay on both the first-boot and re-save paths.
- Machine checks (2026-10-28): lint 0/0, index regenerated, native suite 371/371 (new `test_screen_sleep`
  plus the store/form extensions). All four display envs built SUCCESS with no `.dram0.bss` overflow
  (cardputer, m5stickc_plus2, t_embed, cyd_3248s035c).
- **DoD scenarios met on device (M5StickC Plus2, 2026-10-28):** erased, flashed, provisioned via the portal
  with `screenDimAfterSec=5` / `screenOffAfterSec=10`, associated and hunting. `verify:dim` PASSED: the
  backlight stepped `[DIM] stage=active -> dimmed -> off` and serial output continued after the off
  transition (the loop never paused: the backlight-only claim, scenario 1 and 3). The operator confirmed the
  panel physically dims to a readable level at 5 s, goes dark at 10 s, and jumps back to full on a BtnB press
  or a capture (scenario 2). `artifacts/0094-dim.txt` holds the `[DIM]` transitions (identifier-free, privacy
  guard clean). No deferred on-device audit remains.
