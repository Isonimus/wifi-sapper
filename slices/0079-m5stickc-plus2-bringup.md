---
id: '0079'
title: "M5StickC Plus2 board bring-up + per-board Maintenance entry (applies ADR-0078)"
type: slice
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Goal

Bring up the **M5StickC Plus2** as the second supported board, the first step of slice-8's
hardware broadening (LEDGER), and make it the board that makes ADR-0057 decision 7 concrete: it has
no TCA8418 keyboard, so its Maintenance entry is a side-button hold rather than an `m` keypress. This
slice applies ADR-0078 (the compile-time, `InputKind`-selected entry seam) and adds the Stick's Board
Profile, its LovyanGFX config, and the power-hold bring-up the Stick needs to stay alive off USB.

It also makes the Stick the first **PSRAM** board in the tree (ESP32-PICO-V3-02, 2 MB PSRAM), so it is the
board that forces **ADR-0080** (per-board large-buffer memory placement): the classic ESP32's static-DRAM
budget cannot hold the two ~28 KB cracked manifests the S3 Cardputer carries in `.bss` (a measured 28816-byte
`.dram0.bss` overflow), so on this board they are placed in PSRAM at runtime. This slice applies that policy.
The Stick is also the board the SUB1->SUB2 migration proof (LEDGER) was re-scoped to: unlike the no-PSRAM
Cardputer, a pre-0076 build can alloc the old 16 KB response buffer here, so `markSubmitted` can fire and
write a real `SUB1` store to migrate. That migration proof stays opportunistic and is not in this slice's
Definition of Done.

Board facts (profile pins, LGFX panel parameters, the two on-device-measured gotchas) are reused from
the on-device-tested `esp32-dragon-ball-radar` M5StickC Plus2 HAL rather than derived fresh, per the
LEDGER's instruction.

This slice does **not** deliver the other slice-8 boards (CYD / T-embed / bare-ESP32), per-board memory
or palette tuning, or full TCA8418 text entry; those stay open on the ledger.

## Definition of Done

- **Given** a `m5stickc_plus2` build flashed to the Stick
- **When** it boots
- **Then** the product splash renders correctly on the 240x135 ST7789v2 panel (right geometry, offsets,
  colour, backlight on), and the panel dump is captured for review

- **Given** the Stick running on its internal battery (no USB)
- **When** the firmware has booted and the operator releases the power button
- **Then** the device stays powered (the GPIO4 hold line is asserted early in `setup()`), rather than
  powering off on release

- **Given** `decideButtonHold` and a fresh `ButtonHoldState`
- **When** a press is seen, held across `kMaintenanceHoldMs`, kept held, then released
- **Then** it returns `true` exactly once (on the tick the continuous-hold duration first reaches the
  threshold), never before the threshold, never again while still held, and the state resets on release
  so a later hold fires again; a bounce (release before the threshold) never fires

- **Given** a provisioned Stick in the `Ready` (hunting) phase with recovered results on LittleFS
- **When** the operator holds the side button (BtnB) for >= 2 s
- **Then** the device persists the reboot-intent and restarts, the clean boot enters `phase=maintenance`,
  the `Sapper-Maint-XXXX` SoftAP comes up, and the dashboard serves the recovered PSKs (the ADR-0039 /
  ADR-0057 Maintenance path, now reached by a button hold on a keyboard-less board)

- **Given** the reference Cardputer ADV build (`InputKind::Keyboard`)
- **When** `maintenanceEntryRequested()` is polled in `loop()`
- **Then** it uses the keyboard path unchanged, so the reference board's `m`-keypress entry still works
  (no regression from routing entry through the new seam)

- **Given** the `m5stickc_plus2` build (a PSRAM board, `BOARD_HAS_PSRAM`)
- **When** it is linked and flashed
- **Then** it links within the classic-ESP32 static-DRAM budget (the 28816-byte `.dram0.bss` overflow is
  gone, both ~28 KB cracked manifests placed in PSRAM per ADR-0080), and on boot `psramFound()` is true and
  the hunt and Maintenance manifests allocate in PSRAM (no `[FATAL] ... PSRAM allocation failed`)

## Design

The decision, the three forks (virtual interface / compile-time dispatch / inline branch), and why
compile-time dispatch on `InputKind` was chosen are in ADR-0078. This slice is the first application.

**What existing code was considered.**

- **Reused, from `esp32-dragon-ball-radar` (on-device-tested):** the `LGFX_M5StickC_Plus2` panel
  parameters (ST7789 on HSPI, sclk 13 / mosi 15 / dc 14, cs 5 / rst 12 / bl 27, offset 52/40, inverted,
  rgb_order false, rotation 1), the button GPIOs (BtnA 37, **BtnB 39**, input-only pads with external
  pull-ups so `pinMode(INPUT)` not `INPUT_PULLUP`, pressed = LOW), the battery ADC on **GPIO38** (their
  measurement: GPIO34 read full-scale, 38 reads a real cell through the 1:2 divider), and the GPIO4
  power-hold line. These are measured facts on this exact hardware, so porting them is recording a
  measurement, not guessing (ADR-0002 §5, the same stance as the Cardputer's M5GFX-derived pins).
- **Reused, from this repo:** ADR-0057's board-agnostic intent path (`RTC_NOINIT` word,
  `consumeMaintenanceIntent`, `decideBootPhase`) unchanged; the pure/impure split discipline
  (ADR-0057 decision 6); the `lgfx_cardputer.h` config shape and the `active_board.h` /
  `BoardProfile` model (ADR-0002); the whole downstream Maintenance stack (dashboard, SoftAP hardening,
  controls) untouched.
- **Deliberately NOT reused:** the dragon-ball `PressGesture` class. Its long-press is a 1000 ms app
  input action with different latch/consume semantics, not a one-shot-on-cross gesture feeding an RTC
  reboot. A purpose-built pure `decideButtonHold` (host-tested, 2000 ms maintenance threshold) mirrors
  ADR-0057's choice to write a purpose-built TCA8418 driver rather than drag in M5Cardputer, and keeps
  the pure decision on the native lane. Also **not reused:** M5Unified / M5GFX, for the ADR-0002 /
  ADR-0057 reason (LovyanGFX-only stack).

**The entry seam.** `maintenanceEntryRequested()` is a thin dispatcher `loop()`'s `Ready` branch calls
in place of the direct `g_keyboard.maintenanceKeyPressed()`. It selects the per-board body with
`if constexpr (kActiveBoard.input == InputKind::Keyboard / Buttons)` over the constexpr profile field
(ADR-0002 capability branching, not board identity). The keyboard arm returns
`g_keyboard.maintenanceKeyPressed()`; the button arm reads `digitalRead(bootButtonPin) == LOW` and feeds
the pure `decideButtonHold(pressed, millis(), kMaintenanceHoldMs, g_maintenanceButtonHold)`. The button's
`pinMode(bootButtonPin, INPUT)` is set once on entering `Ready`, beside the existing keyboard
`begin()` gate; a board with `bootButtonPin == -1` under `InputKind::Buttons` has no entry (compile-safe,
returns false).

**Power-hold** is a new `BoardProfile` fact `powerHoldPin` (`int8_t`, `-1` default), a board pin-fact in
the sense ADR-0002 already defined ("the GPIO/bus pins the engine and peripherals need"), not a new
decision. `setup()` asserts it HIGH early when `>= 0`; the Cardputer sets `-1` (USB-powered, no latch),
so its boot is unchanged.

**PSRAM / platform.** A new `[env:m5stickc_plus2]` (board `esp32dev`, ESP32-PICO, `-DBOARD_HAS_PSRAM`, no
`ARDUINO_USB_*` flags) and a matching `[env:m5stickc_plus2_testhooks]` mirroring the Cardputer hooks env,
so the on-air verifies can force Maintenance (`SAPPER_TEST_MAINT`) and seed creds exactly as on the
Cardputer.

**Memory placement (applies ADR-0080).** The Stick's device link failed on a measured 28816-byte
`.dram0.bss` overflow: two ~28 KB `CrackedManifest` objects (the hunt mirror in `hunt_loop.cpp`, the
Maintenance mirror in `main.cpp`) sit in `.bss`, which the S3 Cardputer absorbs but the classic ESP32's
tighter static-DRAM budget cannot. The mechanism was chosen by measuring the precompiled libs' sdkconfig,
not assumed: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` is **unset** (so static `EXT_RAM_BSS_ATTR` cannot
link), but `CONFIG_SPIRAM_USE_MALLOC=y` is set, so each manifest object is `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
+ placement-new'd on `BOARD_HAS_PSRAM` boards and kept as today's static `.bss` object on no-PSRAM boards,
selected by `#ifdef`. The pure `CrackedManifest` policy and its tests are untouched (placement is the
caller's concern). A null PSRAM alloc fails loud, never a silent DRAM fallback. See ADR-0080 for the full
decision, including why static-attribute placement is deferred to the shared patched-libs initiative (LEDGER).

**Verify-script reuse (DRY).** The serial assertions of `verify:device` (splash dump) and
`verify:maintenance` (`phase=maintenance`, the `[MAINT]` banner, the still-answering ping/state) are
board-agnostic, so those two scripts gain an optional board/env parameter (defaulting to the Cardputer
env) rather than spawning per-board copies. No new `scripts/*-verify.mjs`, so no R11 wiring churn.

## Verification

- **test/test_button_hold** (native lane): the pure `decideButtonHold` state machine. Rising edge starts
  the timer; a press shorter than `kMaintenanceHoldMs` never fires; the tick that first reaches the
  threshold fires exactly once; still-held does not re-fire; release resets so a later hold fires again;
  a pre-threshold bounce never fires. Proves DoD scenario 3 (the pure gesture). Fails before the
  implementation, passes after.
- **verify:device on the `m5stickc_plus2` env** (ADR-0004 lane 3, on attached hardware, artifact
  committed): a normal build dumps the splash; the dump plus an eyeball on the physical panel confirm
  geometry/offsets/colour/backlight. Proves DoD scenario 1.
- **verify:maintenance on the `m5stickc_plus2_testhooks` env** (ADR-0004 lane 3, on hardware, artifact
  committed): forced entry (`SAPPER_TEST_MAINT=1`) proves the downstream Maintenance path on the second
  board: `phase=maintenance`, the `Sapper-Maint-XXXX` AP, and the dashboard serving recovered PSKs.
  Proves the downstream half of DoD scenario 4. Artifact redacted per §4 #25.
- **Physical button-hold -> reboot into Maintenance** (on hardware, review-only): the operator holds
  BtnB >= 2 s on a provisioned Stick and observes `[MAINT] ... rebooting into Maintenance` then
  `phase=maintenance`. A physical hold cannot be driven headlessly, so this is review-only exactly as
  §4 #21 treats the Cardputer's physical `m` keypress; the pure gesture is the machine-checked half.
  Proves the entry-gesture half of DoD scenario 4.
- **Power-hold** (on hardware, review-only): the Stick keeps running on battery after the power button is
  released. Proves DoD scenario 2.
- **Cardputer regression** (existing `verify:maintenance` on the `cardputer_testhooks` env, host build
  green): the reference board's keyboard entry still reaches Maintenance through the seam. Proves DoD
  scenario 5.
- **M5StickC Plus2 device link** (ADR-0080, machine-checked on every Stick build): an over-budget
  `.dram0.bss` is a hard link failure, so a green `pio run -e m5stickc_plus2` *is* the proof the overflow is
  gone (both cracked manifests moved to PSRAM). Proves the link half of the PSRAM DoD scenario.
- **On-device PSRAM boot proof** (on hardware, review-only): `psramFound()` true and the cracked-manifest
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)` succeeds (no `[FATAL] ... PSRAM allocation failed`), with the hunt and
  the Maintenance dashboard both reading the mirror correctly. Doubles as the SUB1->SUB2 migration proof
  (LEDGER). Proves the boot half of the PSRAM DoD scenario.

## As built

Shipped on 2026-10-27 as the second supported board. Every Definition-of-Done scenario was met, and the
board facts were ported from the on-device-tested dragon-ball-radar HAL as planned.

**Splash on the panel (scenario 1):** `verify:device` on the `m5stickc_plus2` env dumped the product
splash, and an eyeball on the physical ST7789v2 confirmed geometry (240x135 at rotation 1), offsets
(52/40), colour, and backlight. The `[STATE]` line reported `disp=240x135`.

**Power-hold on battery (scenario 2):** with GPIO4 asserted HIGH early in `setup()`, the Stick stayed
powered on its internal cell after the power button was released. Confirmed on battery (USB removed).

**The pure gesture (scenario 3):** `test/test_button_hold` covers `decideButtonHold`: it fires exactly
once on the tick the continuous hold first reaches `kMaintenanceHoldMs` (2000 ms), never before, never
again while held, and resets on release so a later hold fires again; a pre-threshold bounce never fires.
Green on the native lane (part of the 340-case suite).

**BtnB hold into Maintenance (scenario 4):** on a provisioned, hunting Stick, a >= 2 s BtnB hold persisted
the reboot-intent and restarted into `phase=maintenance`; the `Sapper-Maint-XXXX` SoftAP came up and the
dashboard served the recovered PSKs. `verify:maintenance` on the `m5stickc_plus2_testhooks` env proved the
downstream half (forced entry via `SAPPER_TEST_MAINT`); the physical hold is the review-only half, as
§4 #21 treats the Cardputer's physical `m` keypress.

**Cardputer regression (scenario 5):** routing entry through `maintenanceEntryRequested()` left the
reference board's keyboard `m`-press path unchanged (the `if constexpr` keyboard arm), confirmed by the
Cardputer's `verify:maintenance` still reaching Maintenance.

**PSRAM link and boot (scenario 6):** `pio run -e m5stickc_plus2` links green, so the measured 28816-byte
`.dram0.bss` overflow is gone (both ~28 KB cracked manifests moved to PSRAM per ADR-0080). On device,
PSRAM placement was proved by heap-caps accounting at both call sites, not merely by the absence of a
`[FATAL]`: `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)` (the `[STATE]` `heap_max`) sat at 2064372 B
before the manifest allocation and 2031604 B after, a drop of exactly 32768 B, while `heap_free` (DRAM)
held steady (~119724 B on the hunt path, ~110336 B in Maintenance). A ~2.06 MB largest block is impossible
on the classic ESP32's internal DRAM alone, which confirms PSRAM is live; the 32768 B drop landing on the
PSRAM-backed `MALLOC_CAP_8BIT` heap while DRAM barely moves confirms the manifest was placed in PSRAM, not
DRAM. No `[FATAL] ... PSRAM allocation failed` on either the hunt or the Maintenance path.

**Deviations and bench notes.**

- The `[env:m5stickc_plus2_testhooks]` env was built DRY: a shared `[testhooks]` platformio section (the
  full `SAPPER_TEST_*` probe-macro set plus the two `extra_scripts`) that both testhooks envs reference,
  rather than hand-duplicating the Cardputer's macro set on the Stick. The first attempt duplicated a subset
  and failed to compile (each `*_probe.cpp` references its macro unconditionally under `SAPPER_TEST_HOOKS`);
  the shared section is the root-cause fix and stops the two envs drifting again.
- A bench-only M5 power quirk was measured and documented in the board header: with USB connected, a
  power-button long-press sleeps the unit (screen off, green power LED lit) rather than powering it off, and
  the next wake does not restart the CPU cleanly until an EN reset arrives (opening a serial monitor pulses
  one). This is documented M5 behaviour below firmware (`setup()` never runs in the hung window), confirmed
  harmless by a clean cold boot on battery with USB removed; the appliance runs headless on battery in the
  field.

**SUB1->SUB2 migration (opportunistic, not in this slice's DoD):** the Stick is the first board whose
pre-0073 build could write a real `SUB1` submitted store (a pre-0076 build can allocate the old 16 KB
response buffer here, unlike the no-PSRAM S3), so it is where the LEDGER's deferred submitted-half
migration proof can finally run. That proof stays operator-run and opportunistic; it was not gated into
this slice's Definition of Done.
