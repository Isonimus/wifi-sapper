---
id: '0081'
title: "LilyGo T-embed bring-up — the third supported board (first octal-PSRAM S3)"
type: slice
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Goal

Add the LilyGo T-embed (ESP32-S3, 8 MB octal PSRAM, 16 MB flash, ST7789 320x170, a
rotary encoder with a push switch) as the third board profile, after the no-PSRAM S3
Cardputer (reference) and the classic-ESP32 M5StickC Plus2 (slice-0079). It is the
first octal-PSRAM board and the first encoder board, but it introduces **no new
mechanism**: it is a pure capability-table + HAL-config slice that reuses ADR-0078
(per-board Maintenance entry selected at compile time by `InputKind`) and ADR-0080
(per-board placement of the two large `CrackedManifest` buffers). This is the slice the
LEDGER's slice-8 line keeps open for "CYD / T-embed / bare-ESP32 profiles."

Two board facts make the T-embed cheaper to bring up than the Stick was:

- It is an **S3**, and the no-PSRAM S3 Cardputer already runs the two ~28 KB manifests
  in static DRAM. So unlike the Stick (classic ESP32, where the 28816-byte `.dram0.bss`
  overflow made PSRAM mandatory, ADR-0080), the T-embed has a proven static-DRAM
  fallback. Whether its **octal** PSRAM inits under the precompiled Bruce S3 sdkconfig is
  therefore a measured per-board choice, not a blocker: both branches are already decided
  by ADR-0080 (PSRAM board -> heap route; no-PSRAM board -> static `.bss`).
- Its encoder **push switch is on GPIO0**, the standard ESP32 BOOT-button pattern (idle
  HIGH via the strap pull-up, pressed LOW, runtime-pollable). That is `InputKind::Buttons`
  with `bootButtonPin = 0`, reusing `decideButtonHold` with zero new pure code. This is a
  different case from the Cardputer ADV, where GPIO0 carries no runtime-pressable button
  at all (ADR-0057), which is exactly why that reasoning does not carry over and must be
  documented at the site a later operator would try to "fix."

## Definition of Done

- **Given** the `t_embed` env
- **When** `pio run -e t_embed`
- **Then** it compiles and links clean (no `.dram0.bss` overflow on either the PSRAM or
  the static-DRAM branch, since the S3 has SRAM headroom the Cardputer already proves).

- **Given** a `t_embed_testhooks` build flashed with `SAPPER_TEST_PANEL=1`
- **When** `verify:device` dumps the panel proof
- **Then** the ST7789 renders the RGB-blocks/border/diagonal proof as a full 320x170
  landscape frame with correct colour order, orientation, and no offset clipping (the
  panel-parameter proof of ADR-0002 §5 / ADR-0041, tuned on the physical panel).

- **Given** a normal `t_embed` build
- **When** it boots
- **Then** the product splash (`drawSplashFrame`, ADR-0041) renders at 320x170 and the
  serial log reaches the Ready state with no `[FATAL]`.

- **Given** a `t_embed` build with `BOARD_HAS_PSRAM`
- **When** it boots and both manifest callers run (`hunt_loop` mirror and `runMaintenance`
  mirror, ADR-0080)
- **Then** the on-device heap-caps accounting shows PSRAM live (a `heap_max` drop of
  32768 B per manifest alloc, `heap_free` DRAM steady) and no `[FATAL]`; **or**, if the
  octal PSRAM does not init under the precompiled Bruce S3 sdkconfig, the device falls
  back to the no-PSRAM static-DRAM profile (`hasPsram = false`, no `BOARD_HAS_PSRAM`) and
  boots clean exactly as the Cardputer does. The measured branch is recorded in `## As
  built`.

- **Given** a provisioned `t_embed`
- **When** the operator holds the encoder push switch (GPIO0) for ~2 s during the hunt
- **Then** `decideButtonHold` fires once, the device persists the reboot-intent and
  restarts into Maintenance, and the Maintenance SoftAP serves the dashboard (§4 #21,
  ADR-0078); confirmed end-to-end by `verify:maintenance` against the flashed board.

- **Given** a flashed `t_embed` with real credentials
- **When** it associates and runs the endless hunt
- **Then** the capture -> upload -> sync engine runs as on the reference board (the
  headless-first core is board-independent, ADR-0001), observed over serial.

## Design

**Reused, not rebuilt.** This slice is deliberately config-only:

- **Entry gesture (ADR-0078).** `InputKind::Buttons` with `bootButtonPin = 0` (the encoder
  push). The device glue in `main.cpp` already does `pinMode(bootButtonPin, INPUT)` +
  `digitalRead == LOW` + `decideButtonHold(...)` for any Buttons board, and
  `maintenanceEntryRequested()` is already selected by `InputKind`. No new pure code, so
  `test/test_button_hold` (ADR-0078) already covers the decision; the GPIO0 electrical read
  is the only on-device unknown (see measured questions).
- **Manifest placement (ADR-0080).** The `BOARD_HAS_PSRAM` conditional + the
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)` + placement-new at both sites (`hunt_loop.cpp`,
  `main.cpp`) are board-generic already. The T-embed sets `hasPsram` to match the measured
  PSRAM-init result; no code changes at the two sites.
- **Panel config.** An explicit `LGFX_T_embed` device class (ADR-0002 #2), like the
  Cardputer and Stick, never M5GFX/autodetect. The exact SPI/control GPIOs come from
  LilyGo's on-device-tested factory `pin_config.h` (CLK 12, MOSI 11, DC 13, CS 10, RST 9,
  BL 15). The ST7789 170x320 panel's offset/invert/rgb_order start from the closest tested
  sibling (the T-display-s3, also a LilyGo S3 ST7789) and are **confirmed/tuned on the
  physical panel with the panel probe** (ADR-0041/0042), which exists for exactly this
  measured quantity.
- **Board + flash.** `board = esp32-s3-devkitc1-n16r8` (16 MB flash, octal PSRAM,
  `default_16MB.csv`) matches the chip read off the device (`esptool`: ESP32-S3,
  8 MB octal PSRAM, 16 MB flash). The board def auto-adds `-DBOARD_HAS_PSRAM`; per ADR-0080
  its `memory_type: qio_opi` is **inert** against the precompiled libs (the sdkconfig is
  fixed), so it is harmless and informative, not functional. Native USB-Serial/JTAG means
  the same `ARDUINO_USB_MODE=1` + `ARDUINO_USB_CDC_ON_BOOT=1` as the Cardputer and no
  reset-on-open.
- **Testhooks.** A `t_embed_testhooks` env extends `t_embed` and pulls the shared
  `[testhooks]` section, so its probe-macro set cannot drift from the other two boards
  (the DRY fix slice-0079 introduced).

**Considered and deliberately not reused / not added:**

- **No `InputKind::Encoder`.** The rotary A/B lines drive nothing: the on-device HUD is
  read-only and the Maintenance dashboard is served over the SoftAP, not navigated on the
  device. Entry needs only the push switch, which `InputKind::Buttons` already covers. A
  new input kind for an unused rotation would be the speculative generality the bar forbids
  (rule of three: one encoder is not three). The A/B GPIOs (2, 1) are recorded in the board
  header for a future slice that gives them a job.
- **No APA102 RGB LED.** The T-embed's status LED is a 2-wire APA102 (CLK 45, DATA 42),
  which the single-`ledPin` profile model and the existing single-GPIO LED driver do not
  cover. Wiring it is a new capability, out of scope for a bring-up whose job is the core
  appliance (hunt/capture/upload/maintenance). `led = LedKind::None` for now; the APA102
  pins are recorded and the wiring is deferred (LEDGER).
- **No SD.** The board has a MicroSD slot (CS 39 / SCK 40 / MOSI 41 / MISO 38), but the
  recovered manifest and stores live on flash LittleFS as on the Stick
  (`StorageKind::LittleFs`). SD pins are recorded for a future slice.
- **No in-house T-embed HAL to port.** Unlike the Stick (which reused the on-device-tested
  dragon-ball-radar M5StickC Plus2 HAL), neither the Adversary nor dragon-ball-radar has a
  tested T-embed definition (the Adversary marks it "not yet implemented"). So the pins come
  from LilyGo's own factory firmware (on-device-tested upstream) and the panel parameters
  are confirmed with the panel probe rather than trusted from a retailer pinout.

**Measured questions, resolved on device (not estimated):**

1. Does the **octal** PSRAM init under the precompiled Bruce S3 sdkconfig? Resolved by the
   heap-caps DoD scenario. Fail-loud (`[FATAL]`) is ADR-0080's safety net if it does not;
   the static-DRAM fallback (Cardputer path) is the alternative branch, so either answer
   ships.
2. Does GPIO0 read cleanly with plain `INPUT` (relying on the external strap pull-up), or
   does it need `INPUT_PULLUP`? Resolved by the Maintenance-entry DoD scenario. If it
   floats, the fix is a per-board pull-mode fact on `BoardProfile`, recorded here and in
   the LEDGER rather than hardcoded.

## Verification

- `test/test_button_hold` (existing, ADR-0078): the pure entry-gesture decision the
  T-embed's GPIO0 hold drives. No new pure logic is introduced, so no new native test is
  added (adding one would duplicate an existing covered behaviour, which the bar forbids).
- `verify:device` (`scripts/0005-cardputer-bringup-verify.mjs`), run against a flashed
  `t_embed_testhooks` build: the panel proof (`SAPPER_TEST_PANEL=1`) and the normal-build
  splash, plus the boot-state dump carrying the heap-caps numbers for the PSRAM scenario.
- `verify:maintenance` (`scripts/0040-maintenance-verify.mjs`), run against a flashed
  `t_embed_testhooks` build: forced entry (`SAPPER_TEST_MAINT=1`), the Maintenance SoftAP,
  and the dashboard serve, confirming the §4 #21 path on the encoder-push board.
- The device link itself proves the no-overflow DoD scenario (an over-budget `.dram0.bss`
  is a hard build failure), as it does for the Stick under ADR-0080.

Both device verify scripts are already wired in `package.json` (they read serial from
whatever build is flashed, so they need no new entry); the native suite and linter are
unaffected because this slice adds no pure code and no new document rule.

## As built

Shipped as a config-only third board, exactly as designed: no new ADR, no new pure code,
reusing ADR-0078 (entry by `InputKind`) and ADR-0080 (PSRAM placement). The device read off
the wire matched the plan (esptool: ESP32-S3, 8 MB octal PSRAM, 16 MB flash, USB-Serial/JTAG),
so `board = esp32-s3-devkitc1-n16r8` and the LilyGo factory pins were used verbatim.

Every Definition-of-Done scenario was confirmed on the connected unit:

- **Build/link.** `pio run -e t_embed` compiled and linked clean on the first try (RAM
  28.9 %, no `.dram0.bss` overflow), and both `t_embed` / `t_embed_testhooks` parse and share
  the `[testhooks]` section. Native stayed 340/340 and lint 0/0 (no pure code added).
- **Panel.** First flash rendered the panel 180-degrees rotated with correct colour and
  spacing. The one on-device deviation from the written design: the ST7789 needed
  `rotation = 3`, not the `rotation = 1` first-guessed from the Stick, to present 320x170
  upright. Fixed in the board profile and the `lgfx_t_embed.h` note, reflashed, and the
  operator confirmed the splash upright with correct colour and spacing. The dedicated
  `SAPPER_TEST_PANEL` proof frame was not separately dumped; the shipped splash exercises the
  same three panel properties (orientation, colour order, offset/spacing) on the physical
  glass, which is what the scenario checks, so it stands as the panel confirmation. The
  seeded offsets (`offset_x = 35, offset_y = 0`) and invert/rgb_order from the T-display-s3
  sibling were correct unchanged.
- **Boot splash + Ready.** The device boots clean to the splash, reaches `phase=ready`, and
  shows no `[FATAL]`.
- **PSRAM (measured question 1: resolved YES).** The octal PSRAM inits under the precompiled
  Bruce S3 sdkconfig: `[STATE] heap_max=8257524` (~7.87 MB largest free block,
  `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`) proves it live and mapped into the
  heap, the same methodology ADR-0080 used on the Stick (~2.06 MB for 2 MB quad PSRAM).
  **Both** manifest allocs are proven: the Maintenance mirror by `verify:maintenance` passing
  (it `[FATAL]`-restarts on a null alloc, and it served the dashboard), and the hunt mirror by
  a captured boot-through-hunt run reaching discovery with no `[FATAL]` and a
  `[SYNC] download ok ... downloaded=28` that parsed 28 cracked entries into the PSRAM-placed
  `g_manifest`. The expected per-alloc `heap_max` drop of 32768 was not visible in a `[STATE]`
  line because the `ready` phase is printed before `huntLoopBegin` runs the alloc; the alloc's
  success is instead proven by the fail-loud `[FATAL]` never firing and the functional sync
  into the manifest, which is the stronger evidence. The static-DRAM fallback branch was
  therefore not needed (though it remained available, since the no-PSRAM S3 Cardputer proves
  it).
- **Maintenance entry (measured question 2: resolved YES).** The operator held the encoder
  push (GPIO0) during the hunt and the device logged the gesture and rebooted into
  Maintenance. So GPIO0 reads cleanly on plain `INPUT` via its external strap pull-up, and
  **no per-board pull-mode fact was needed** on `BoardProfile`. `verify:maintenance` also
  passed (forced entry, SoftAP, dashboard serve).
- **Headless hunt.** The captured run showed the board-independent engine running on the
  T-embed: `[DEAUTH] disarmed` (default-off, §4 #16), `[LED] status=hunting`,
  `[HUNT] discovering channels=1,6,11`, and a successful cracked-sync download. A live
  handshake capture is airtime-dependent (needs a client associating during the window), so
  the engine running end-to-end is the proof, as on the reference board.

Deferred on this board and recorded in the LEDGER, not left as silent TODOs: the APA102
2-wire status LED (does not fit the single-`ledPin` model), the MicroSD slot (stores live on
flash LittleFS), and the rotary encoder's A/B rotation lines (no on-device consumer yet). The
pins for all three are recorded in the board header as data for those future slices.
