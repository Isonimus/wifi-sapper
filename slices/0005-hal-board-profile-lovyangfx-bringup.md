---
id: '0005'
title: "HAL skeleton, Board Profile, and LovyanGFX bring-up on the Cardputer"
type: slice
status: accepted
date: 2026-09-17
supersedes: []
superseded_by: []
---

## Goal

Stand up the portable foundation every later slice builds on, and prove it on the reference
board (M5Stack Cardputer, ESP32-S3, no PSRAM). This slice delivers: the `platformio.ini`
toolchain — pioarduino platform + Bruce's patched `esp32-arduino-libs` + the
`weaken_deauth_pre.py` link step (ADR-0001), so the deauth-capable toolchain is correct from
day one even though no deauth code ships yet; a `native` Unity test env; the `constexpr
BoardProfile` capability descriptor selected by one `SAPPER_BOARD_*` flag (ADR-0002 #1); a
capability-queried display HAL with both its real and null resolutions (ADR-0002 #4); the
`LGFX_Cardputer` config class and `LGFX_Sprite` canvas brought up *through LovyanGFX
directly, not M5GFX* (ADR-0002 #5); and the **observation half** of the serial control
channel — the pure parser plus `ping`/`state`/`dump`, unflagged, read-only (ADR-0003 #2).
It also creates `package.json` and the first verify script, establishing all three
verification lanes (ADR-0004).

This closes the roadmap's `slice-1` item and delivers the observation half of the
serial-channel item; the stimulus/`SAPPER_TEST_HOOKS` half of that item stays open in
`LEDGER.md` until there is an engine to stimulate (slice-3/4).

## Definition of Done

**Scenario A — the hardware-free foundation is unit-tested.**
- **Given** the `native` env, with no board attached
- **When** `npm run test:native` (`pio test -e native`) runs
- **Then** the serial-command parser tests (grammar, over-length refusal, `CommandReader`
  accumulation) and the Board Profile tests (Cardputer profile reports `hasDisplay=true`,
  `input=keyboard`, `storage=sd`, `hasPsram=false`; the display HAL resolves to the null
  display when `hasDisplay=false` and to the real one when true) all pass.

**Scenario B — the firmware builds against the real toolchain.**
- **Given** the `cardputer` env pinned to the pioarduino platform and Bruce's libs
- **When** `npm run build:boards` (`pio run -e cardputer`) runs
- **Then** the `weaken_deauth_pre.py` pre-action reports the symbol weakened and the firmware
  compiles and links to a `.elf` with no error.

**Scenario C — the panel initialises and renders correctly on hardware.**
- **Given** a Cardputer flashed with this slice's firmware
- **When** it boots and the verify script requests a canvas dump over serial
- **Then** no `[FATAL]` line appears, the `LGFX_Cardputer` panel initialises at 240×135, the
  scanner-view bring-up frame is rendered, and the dumped `LGFX_Sprite` reconstructs to a PNG
  whose geometry, colours, and orientation match the rendered frame (no offset, mirror, or
  byte-order error) — confirming the empirically-open panel parameters ADR-0002 §5 deferred
  here.

**Scenario D — the observation channel answers without mutating state.**
- **Given** a flashed, booted Cardputer
- **When** the verify script sends `ping`, then `state`
- **Then** the device replies `[CMD] pong` and a `[STATE]` line carrying free heap, largest
  contiguous block, and display geometry; a second `state` reports the same free-heap figure
  within noise, evidencing the observation path allocated nothing and mutated no state
  (ADR-0003 invariant #1).

## Design

**Toolchain (`platformio.ini`).** The Adversary's `env_embedded`/`env:cardputer`/`env:native`
structure is reused as the proven shape: pioarduino `platform-espressif32` **55.03.34**,
`platform_packages` pinned to Bruce's `bruce_esp32-arduino-libs-20251205-131242.zip`
(idf-release_v5.5), `board = m5stack-stamps3`, `board_build.flash_mode = qio` with **no**
`memory_type` override (the StampS3 has no PSRAM; `qio_opi` boot-fails loudly — the
Adversary's hard-won comment is carried over). `extra_scripts = pre:scripts/weaken_deauth_pre.py`.
The M5-specific `lib_deps` (`M5GFX`, `M5Unified`, `M5Cardputer`) are **deliberately not
reused** — ADR-0002 §5 brings the panel up through LovyanGFX directly — replaced by
`lovyan03/LovyanGFX`. `ArduinoJson` is kept; the RF/IR/BLE/RFID libs are dropped (out of
scope, ADR-0001). The `native` `build_src_filter` is reset to this repo's headless tree
(`core/`, `hal/` pure parts, the serial parser), not the Adversary's module list.

**Deauth pre-script.** `scripts/weaken_deauth_pre.py` and `scripts/weaken_deauth_symbol.sh`
are ported verbatim — they derive `objcopy` from `$CC` and idempotently weaken
`ieee80211_raw_frame_sanity_check` in the precompiled lib. Harmless with no `deauth.cpp`
present (they weaken a library symbol regardless); wiring the toolchain now means slice-3's
deauth links without a toolchain scramble later.

**Board Profile + display HAL.** A `constexpr BoardProfile` (ADR-0002 #1) replaces the
Adversary's scattered `TARGET_*` `#ifdef`s and its `canvas_types.h` `using Canvas = M5Canvas`.
Pin values come from the Adversary's `pins.h` (Cardputer display CS 37 / DC 34 / RST 33 /
BL 38; SD CS 12 / MOSI 14 / MISO 39 / CLK 40; KB_INT 46; BAT_ADC 10). The display HAL is an
interface with two resolutions selected by `profile.hasDisplay`: the real one wrapping an
`LGFX_Sprite`, and a null one that no-ops — so a screenless board compiles no `LGFX_*` class
(ADR-0002 #4). The null-vs-real selection is unit-tested natively with a stub profile; the
screenless board itself arrives in slice-8.

**`LGFX_Cardputer`.** A class deriving from `lgfx::LGFX_Device` (ADR-0002 #2), configuring
the panel, SPI bus, and backlight explicitly. Starting hypothesis: ST7789-family panel,
240×135, SPI, with the pins above; the exact driver, rotation, and x/y offsets are **not
asserted here** — they are confirmed by Scenario C's PNG artifact, which is precisely the
empirical confirmation ADR-0002 §5 routed to this bring-up.

**Serial observation channel.** Adapted from `dupin`'s `src/ui/serial_command.h` — its pure
`parseCommand` + `CommandReader` shape and bounded/refuse discipline are reused (ADR-0003 #4,
#5). The `CommandKind` enum is re-cut for this repo: `Ping`/`State`/`Dump` for the
observation half; the stimulus kinds are absent (they arrive behind `SAPPER_TEST_HOOKS` with
the engine). Dispatch reads only what the device already exposes — heap figures from the IDF,
geometry from the profile, the sprite buffer from the canvas — mutating nothing (ADR-0003
invariant #1). `dump` streams the `LGFX_Sprite` buffer (it lives in RAM and is readable back)
as the deterministic, camera-free artifact source for Scenario C.

**Verification harness (`package.json`).** Created here as the script registry ADR-0004 fixed
(not a Makefile — see ADR-0004): `test:native`, `build:boards`, `verify:device`, plus `lint`
and `index` wrappers over the existing doc scripts, and a `serialport` devDependency for the
verify script.

## Verification

- **Native Unity tests** (Scenario A, D-parser) — `test/test_serial_command` (parser grammar,
  over-length refusal, `CommandReader` accumulation) and `test/test_board_profile` (capability
  queries + display-HAL null/real selection via a stub profile). Wired as `npm run test:native`
  (`pio test -e native`); runs in cloud CI (ADR-0004 lane 1).
- **Board compile** (Scenario B) — `npm run build:boards` (`pio run -e cardputer`) links the
  `.elf` and exercises the `weaken_deauth_pre.py` step; runs in cloud CI (ADR-0004 lane 2).
- **Device verify** (Scenarios C, D) — `scripts/0005-cardputer-bringup-verify.mjs`, wired as
  `npm run verify:device`. Over the serial channel it asserts no `[FATAL]`, `ping`→`[CMD] pong`,
  and a well-formed `[STATE]`, then requests `dump` and writes `artifacts/0005-cardputer-bringup.png`
  plus the captured `[STATE]` for human review. Error-check half is pass/fail; runs on attached
  hardware only, never cloud CI (ADR-0004 lane 3, invariant #5).

## As built

_(filled at merge — the freeze step: what actually shipped, confirming each scenario or
recording the deviation.)_

## Amendment — 2026-09-17: reference device is the Cardputer ADV

The reference board named "M5Stack Cardputer" throughout this slice is specifically the
**Cardputer ADV** — the physical device on hand. The Adversary repo labels this same hardware
as plain "Cardputer" everywhere, so its `m5stack-stamps3` board id, pin map, and display
config (reused here) are in fact the ADV's. Electrically for this slice (display + serial)
the two are the same StampS3, so nothing in the plan changes; Scenario C's bring-up artifact
is captured on the actual ADV.

Frozen 2026-09-17. All four Definition-of-Done scenarios met on the M5Stack Cardputer ADV:

- **Scenario A (native, lane 1)** — `pio test -e native` passes 13/13: the serial parser
  (vocabulary, over-length refusal, `CommandReader` reset) and the Board Profile / display-HAL
  resolution tests.
- **Scenario B (board compile, lane 2)** — `pio run -e cardputer` compiles and links to a
  `.elf` against pioarduino 55.03.34 + Bruce's IDF-5.5 libs, and the `weaken_deauth` pre-script
  leaves `ieee80211_raw_frame_sanity_check` weak (`nm` shows `W`). Static RAM 23.5 KB / 327 KB,
  flash 422 KB.
- **Scenario C (panel, lane 3)** — the `LGFX_Cardputer` panel initialised at 240×135 and the
  bring-up frame rendered correctly on the physical screen (border on all four edges, R/G/B
  blocks in order, corner-to-corner diagonal). The `dump` artifact
  (`artifacts/0005-cardputer-bringup.png`) reconstructs the canvas and proves the render
  pipeline; the physical panel confirmed the parameters.
- **Scenario D (observation channel, lane 3)** — `ping`→`[CMD] pong`; `[STATE]` carried
  `heap_free=287132 heap_max=233460 disp=240x135`, identical across two reads, evidencing the
  observation path mutates nothing.

Deviations from the plan, all recorded here rather than by editing the plan above:

- **LovyanGFX pinned to 1.2.29, not an arbitrary release.** Releases before the Aug-2025 fix
  (e.g. the 1.2.0 first resolved from cache) fail to compile `Bus_RGB.cpp` against ESP-IDF 5.5
  (`gpio_hal_iomux_func_sel` renamed to `gpio_hal_func_sel`). 1.2.29 carries the version-guarded
  fix. A stale `.pio` cache masked the bump once — the lib cache must actually re-resolve.
- **Panel params sourced from M5GFX `board_M5CardputerADV`, not a guess.** Bring-up first
  showed a black screen: the panel inits fine but stays dark until `setBrightness()` is called
  after `init()`, and the bus is `SPI3_HOST`. Both are now in `LGFX_Cardputer` / `LgfxDisplay`.
- **Two verify-script bugs fixed on first hardware run.** RGB565 is stored MSB-first in the
  `LGFX_Sprite` buffer (decode big-endian), and every request must register its serial listener
  *before* sending or the first streamed dump lines land in a handler gap and are dropped.
- **`native` env uses `gnu++17`** so the Board Profile's designated initializers compile.

The reference device throughout is the Cardputer ADV (see the amendment above).
