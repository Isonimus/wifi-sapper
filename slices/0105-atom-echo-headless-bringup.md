---
id: '0105'
title: "M5Stack Atom Echo headless bring-up — the first zero-new-decision board"
type: slice
status: accepted
date: 2026-10-31
supersedes: []
superseded_by: []
---

## Goal

Bring up the **M5Stack Atom Echo** (ESP32-PICO-D4, classic ESP32, 4 MB flash, no PSRAM) as the fifth
supported board: the capture -> upload -> sync engine, provisioning, and the Maintenance phase, running
**headless** with a single RGB status LED and the side button as its only local surfaces. This advances
slice-8's hardware broadening (LEDGER) toward the bare classic-ESP32 target, and it is the first board the
tree can add with **no new architectural decision**: every capability axis it has was already decided for
an existing board. It is classic-ESP32 + no-PSRAM like the CYD (so it reuses ADR-0082's internal-heap
manifest placement), genuinely screenless like the ADR-0001 headless-first core (so it reuses the
ADR-0002 #4 / ADR-0045 null-display path the CYD proved transiently at slice-0083), carries a one-wire RGB
LED like the Cardputer (so it reuses the `LedKind::Rgb` driver), and enters Maintenance by a side-button
hold on the very pin the Stick already uses (so it reuses ADR-0078 verbatim). Its distinct value is not a
new capability but a **real-hardware proof of the screenless appliance**: the `SAPPER_BOARD_HAS_DISPLAY`
guard has only ever shipped absent as a transient state (the CYD before slice-0085 gave it a panel); here
it is a board's permanent, intended configuration.

The 4 MB fit was measured before the slice, not estimated: the CYD (same chip family, same no-PSRAM
internal-heap config, and carrying the full DirectPanel display stack this board omits) builds to a 1.41 MB
`firmware.bin` = 45% of `huge_app.csv`'s 3.00 MB app slot (LEDGER 2026-10-29 measurement). The Atom Echo
compiles no display code, so it lands at or below that, with ~1.7 MB app headroom and the proven 896 KB
LittleFS. No OTA is shipped (deferred, LEDGER), so the single-slot `huge_app.csv` is correct, exactly as on
the CYD.

## Definition of Done

- **Given** the `atom_echo` env with `hasDisplay = false` and no `SAPPER_BOARD_HAS_DISPLAY` flag
- **When** `pio run -e atom_echo` links
- **Then** it compiles and links clean while compiling **no** `LGFX_*` class (the ADR-0002 #4 screenless
  contract, shipping absent as a board's permanent state for the first time); the RAM/Flash figures are
  recorded in `## As built`.

- **Given** `huge_app.csv`'s 3.00 MB single app slot and this board's headless build
- **When** the env links
- **Then** Flash usage is well under half the app slot (predicted <= the CYD's 1.41 MB / 45%, lower because
  no display stack compiles), confirming the 4 MB partition math this slice is predicated on; the exact
  figure is recorded in `## As built`.

- **Given** a no-PSRAM classic ESP32 with `-DSAPPER_MANIFEST_HEAP_INTERNAL` (ADR-0082)
- **When** `pio run -e atom_echo` links
- **Then** it links with no `.dram0.bss` overflow (both static manifests leave `.bss`, the one live
  manifest is placed in the internal DRAM heap), and on device the measured `heap_max` is >> 28 KB,
  recorded in `## As built`.

- **Given** a flashed `atom_echo` on an unprovisioned device
- **When** it boots
- **Then** the serial log reaches `[STATE] phase=provisioning` with `disp=0x0` (the headless NullDisplay),
  **no `[FATAL] display init failed`** line (a screenless board is the designed state, ADR-0045), and the
  captive portal comes up; the measured `heap_max` is recorded in `## As built`.

- **Given** a provisioned `atom_echo` running the hunt
- **When** the operator holds the side button (GPIO39) for ~2 s
- **Then** `decideButtonHold` fires once, the device persists the reboot-intent and restarts into
  Maintenance, and the Maintenance SoftAP serves the dashboard (§4 #21, ADR-0078), confirmed end-to-end by
  `verify:maintenance` against the flashed board.

- **Given** a flashed `atom_echo` with real credentials
- **When** it associates and runs the endless hunt, and a capture or a cracked-password event drives the
  status LED (the SK6812 on GPIO27, via the `LedKind::Rgb` path)
- **Then** the capture -> upload -> sync engine runs as on the reference board (the headless-first core is
  board-independent, ADR-0001), observed over serial, and the LED lights in its status colour (eyes-on,
  the operator's to run).

- **Given** the release workflow with the new `atom_echo` leg
- **When** it packages the factory image
- **Then** the image is `dio`-headed with bootloader magic `0xE9` read at the classic-ESP32 `0x1000`
  offset (§4 #42, ADR-0095), published as `sapper-atom_echo-factory.bin`, and the web flasher offers the
  board through `manifest-atom_echo.json` (`chipFamily: "ESP32"`, part offset 0).

## Design

**No new decision.** Unlike every prior board bring-up (the Stick forced ADR-0080, the CYD forced
ADR-0082 and later ADR-0084/-0086), this slice records no ADR, because the Atom Echo's capability vector is
the intersection of boards already in the tree. That is the slice's thesis and the reason it clears the
design-first bar on reuse alone. What is new is only *data* (a board profile, an env, a release leg, a
flasher manifest) and the *first permanent* exercise of an already-built guard.

**Reused, not rebuilt.**

- **Manifest placement (ADR-0082).** Classic ESP32 + no PSRAM is the CYD's case exactly:
  `-DSAPPER_MANIFEST_HEAP_INTERNAL=1`, the two static `CrackedManifest` instances leave `.dram0.bss` and
  the one live manifest is `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` + placement-new'd at
  the same two sites (`hunt_loop.cpp`, `main.cpp`), with the `[FATAL]` guard. The pure policy and its host
  tests are untouched (placement is the caller's concern), so the native suite is unchanged by
  construction. This board overflows static `.bss` like the CYD/Stick (same chip family), so the
  internal-heap route is not optional; no re-proof of ADR-0082's overflow measurement is needed (the
  decision is already recorded), the clean link is the §4 #37 machine check.
- **Headless display path (ADR-0001, ADR-0002 #4, ADR-0045).** `hasDisplay = false` + no
  `SAPPER_BOARD_HAS_DISPLAY` resolves the HAL to `NullDisplay` (`begin()` returns true: the designed
  screenless state, not a fault), `hunt_loop` skips the screen surface on `!kActiveBoard.hasDisplay`, and
  the `lgfx_display.*` files guarded at slice-0083 compile no panel class. The CYD exercised this path at
  slice-0083 and booted clean (`disp=0x0`), then slice-0085 gave it a panel and flipped the flag on. The
  Atom Echo is the first board for which the flag stays **permanently absent**: there is no panel at all, so
  none of the ADR-0084 `DirectPanelDisplay` / ADR-0086 `DirectPanelScreenRenderer` machinery compiles
  either (no `SAPPER_DISPLAY_DIRECT_PANEL`). The boot-face path (`drawSplashFrame` /
  `drawProvisioningScreen`, slice-0104) draws through the init-checked `panelAfterInit()` to the
  `NullDisplay`, a no-op, exactly as on the CYD before its panel.
- **Entry gesture (ADR-0078).** `InputKind::Buttons` with `bootButtonPin = 39`, the **same pin and
  electrical profile** as the Stick's BtnB: an input-only pad (no internal pull) the board pulls up
  externally, read as `INPUT`, pressed = `LOW` ([m5stickc_plus2.h](src/config/boards/m5stickc_plus2.h)'s
  measured gotcha). The device glue in `main.cpp` already does the `pinMode(INPUT)` + `digitalRead == LOW`
  + `decideButtonHold(...)` for any `Buttons` board, and `maintenanceEntryRequested()` is already selected
  by `InputKind`. No new pure code, so `test/test_button_hold` already covers the decision; the GPIO39
  electrical read is the only on-device unknown, and it is the lowest-risk kind because the Stick already
  proved this exact pin.
- **RGB LED (`LedKind::Rgb`).** The Atom Echo's single SK6812 on GPIO27 (GRB order, confirmed from the
  device's own ESPHome `esp32_rmt_led_strip` log) is the same one-wire addressable protocol and byte order
  as the Cardputer's WS2812 on G21 ([cardputer.h](src/config/boards/cardputer.h)), so `led = LedKind::Rgb`,
  `ledPin = 27` reuses the `led_driver_esp32.cpp` `Rgb` path unchanged. The on-device unknown is only colour
  fidelity / order, settled eyes-on (the LED is this board's primary local surface, so unlike the CYD's
  deferred LED this one earns its wiring immediately).
- **Board + flash.** `board = esp32dev`, `board_build.mcu = esp32`, `flash_size = 4MB`,
  `partitions = huge_app.csv` (3 MB app + ~896 KB spiffs/data), identical to the CYD env. A USB-UART bridge
  (the Atom Echo's onboard converter, enumerating as `/dev/ttyUSB0`) resets on open like the Stick/CYD, so
  no `ARDUINO_USB_*` flags (unlike the native-USB S3 boards).
- **Board selection + testhooks.** One `#elif defined(SAPPER_BOARD_ATOM_ECHO)` branch in
  [active_board.h](src/config/active_board.h) mapping to `kAtomEchoProfile`, **without** the
  `SAPPER_BOARD_HAS_DISPLAY` define (the headless branch). An `atom_echo_testhooks` env extends `atom_echo`
  and pulls the shared `[testhooks]` section, so its probe-macro set cannot drift from the other boards.
- **Release + flasher (ADR-0095, slice-0102).** A fifth matrix leg in `release.yml`
  (`env: atom_echo`, `chip: esp32`, `boot_offset: '0x1000'`, `flash_size: 4MB`), which the existing dio
  brick-guard binds per §4 #42; a fifth stable `sapper-atom_echo-factory.bin`; `manifest-atom_echo.json`
  (`chipFamily: "ESP32"`, offset 0); the board added to the `pages.yml` fetch loop and the
  [web/index.html](web/index.html) picker (`data-chip="ESP32"`) plus its esptool cheat-sheet line.

**Considered and deliberately not reused / not added:**

- **No display HAL of any kind.** The Atom Echo has no panel, so unlike the CYD it gets neither the
  sprite-backed `LgfxDisplay` nor the `DirectPanelDisplay`: `hasDisplay = false` and both display flags
  absent. Adding a panel config it has no panel for would be the `#error`-guarded mistake ADR-0002 #4
  exists to catch.
- **No battery gauge.** The Atom Echo is USB/Grove-powered with no cell, so `batteryAdcPin = -1` and
  `batteryDividerPermille = 0` -> `BatteryLevel::Unknown` (ADR-0091, measure-don't-estimate): no guessed
  divider, and the level is moot anyway on a board with no HUD to show it.
- **No power latch.** Unlike the Stick's GPIO4 keep-alive, the Atom Echo has no software power-latch
  (USB-powered, no battery power button), so `powerHoldPin = -1`. That `setup()` runs to completion without
  a latch is the one boot assumption to confirm on device (expected; the Cardputer/CYD also declare -1).
- **No SD.** No TF slot; captures/config persist on flash LittleFS as on the Stick/CYD (`storage =
  LittleFs`).
- **The I2S microphone, speaker, and IR are left untouched.** They are real peripherals of the voice-
  assistant hardware but have no consumer in the Sapper; the profile models only what the engine and its
  surfaces use (ADR-0002), so no fields are added for them.
- **No reduced-logging env.** ADR-0101 declined log-stripping for every board on measured grounds (the
  strippable literals are <1% of free flash); a 4 MB board with ~1.7 MB headroom does not change that.

## Verification

- **`pio run -e atom_echo`** — the device link is the machine check for three claims at once: the 4 MB fit
  (Flash % of the 3 MB `huge_app` slot), the internal-heap manifest fix (no `.dram0.bss` overflow, §4 #37),
  and the screenless contract (the build completes with no `LGFX_*` class compiled, ADR-0002 #4). All
  figures recorded in `## As built`.
- **`test/test_button_hold`** (ADR-0078) — the pure Maintenance-entry hold decision, already covering the
  `InputKind::Buttons` GPIO39 gesture this board reuses from the Stick; no new pure code, so no new test.
- **The existing manifest suites** (`test/test_cracked_manifest`, `test/test_submitted_manifest`) and the
  full native suite are unchanged: placement is the device caller's concern, so the pure policy they cover
  is byte-for-byte.
- **On-device headless boot** (not unit-assertable): the flashed build reaches `[STATE] phase=provisioning`
  with `disp=0x0`, no display `[FATAL]`, and a measured `heap_max` >> 28 KB (captured in `## As built`).
- **`verify:maintenance`** against the flashed, provisioned board confirms the GPIO39 hold -> reboot ->
  Maintenance SoftAP path end-to-end (ADR-0078 / §4 #21); the operator's to run, like the Stick/CYD.
- **RGB LED** (not unit-assertable): a capture or cracked-password event lights the SK6812 in its status
  colour; eyes-on, the operator's to run (the `LedKind::Rgb` policy itself is pure and already covered by
  the LED-surface suite).
- **The endless hunt** on a provisioned device (real WiFi + a wpa-sec account): the capture -> upload ->
  sync engine observed over serial, the operator's to run, tracked in the LEDGER alongside the other
  boards' deferred on-air proofs.
- **Release packaging**: the `release.yml` in-step dio guard reads the factory image's bootloader header at
  `0x1000` and asserts magic `0xE9` + flash-mode `0x02` (§4 #42, ADR-0095) for the new leg; the web flasher
  manifest serving its binary same-origin is the slice-0102 Pages path.

## As built

Shipped as designed, and the only new on-device finding was an upload-baud quirk (now pinned in the env).
The board is the first bring-up to need no new ADR: the env, profile, and wiring are all reuse.

- **Build / link (the machine check):** `pio run -e atom_echo` links clean with **RAM 28.8%** (94344 B of
  327680) and **Flash 42.5%** (1337575 B of the 3 MB `huge_app` app slot: ~1.8 MB free), with no
  `.dram0.bss` overflow and no `LGFX_*` class compiled. The 4 MB fit this slice was predicated on holds,
  with ~57% of the app slot free. This lands **level with the CYD** (RAM 28.8% / Flash 42.1%), not below it
  as the Goal predicted: the `LedKind::Rgb` surface this board links (the CYD has `led = None`) roughly
  offsets the display stack the CYD compiles, so "headless is smaller" did not hold, but the fit margin is
  unchanged either way.
- **Upload quirk (the one new finding):** the first flash failed with `A fatal error occurred: The chip
  stopped responding` at esptool's switch to the 460800 write baud. A `--before default-reset` probe
  connected and ran the stub cleanly at 115200 (`Chip type: ESP32-PICO-D4 (revision v1.1)`), confirming
  auto-reset and the cable are fine: the PICO-D4's USB-UART bridge simply cannot sustain the high write
  baud. Pinned `upload_speed = 115200` in the `atom_echo` env (a per-board bridge difference, documented
  at the site; the CYD on the same `esp32dev` board def tolerates the default). Flashing is reliable after.
- **Headless boot:** flashed over the bridge (`/dev/ttyUSB0`), the board boots (classic-ESP32 ROM,
  `mode:DIO`) to `[STATE] phase=provisioning heap_free=245576 heap_max=110580 disp=0x0`, with **no**
  `[FATAL] display init failed` line, the captive portal up (`Sapper-XXXX`, the SoftAP name redacted per
  §4 #25, fixed public password per ADR-0089). `disp=0x0` and the later `[SCREEN] disabled (no display)`
  confirm the headless NullDisplay path on real silicon: the first board to ship `SAPPER_BOARD_HAS_DISPLAY`
  permanently absent. `heap_max=110580` (matching the CYD) is ~4x the 28 KB the manifest needs.
- **Internal-heap manifest (ADR-0082) proven end-to-end:** after provisioning + reboot, the device
  reaches `phase=ready` and the first sync **seeds the internal-heap `CrackedManifest`**: `[SYNC] download
  ok bytes=1636 lines=32`, `[SYNC] first-sync summary imported=32 (seeded silently, not alerted)`, `[SYNC]
  outcome ok=1 first=1 downloaded=32 new=0 malformed=0 overflow=0 storeError=0 fetch=0`, with no `[FATAL]
  ... internal-heap allocation failed`. This is the leg the CYD's slice-0083 run did **not** complete (its
  wpa-sec cracked-download TLS handshake errored), so the Atom Echo is the first board to prove the
  ADR-0082 internal-heap manifest and the wpa-sec cracked-download seed **together** on no-PSRAM classic
  silicon.
- **RGB LED surface (`LedKind::Rgb`, G27):** `[LED] status=hunting` / `status=working` transitions fire
  from the shared LED surface, so the Cardputer's WS2812 driver path drives the Atom Echo's SK6812
  unchanged. The physical colour/order is eyes-on and not yet confirmed (LEDGER).
- **Hunt spine (ADR-0001, board-independent):** `[WEBHOOK] disabled`, `[DEAUTH] disarmed - passive hunt`
  (default-off, §4 #16), `[SCREEN] disabled (no display)`, `[HUNT] discovering channels=1,6,11`, and the
  sync above, all running as on the reference board.
- **Bonus — ADR-0074 filesystem seam on ex-foreign-firmware flash:** the board arrived running ESPHome, so
  its data partition held non-LittleFS bytes that mounted as `esp_littlefs: Corrupted dir pair at {0x1,
  0x0}` / `mount failed (-84)`. The `ensureFilesystemMounted()` seam correctly classified this as a
  **benign first-time format** (`[FS] first-time format of a new or erased partition (no prior data)`), with
  **no** data-lost `[ERROR]` and no reformat breadcrumb, because a fresh install has no `fs_seen` marker:
  ADR-0074 decision 3 (an erased/foreign partition reports the identical failure as a corrupt one, so the
  marker is what distinguishes "lost data" from "never had any") working exactly as designed, on its first
  real foreign-firmware board. Benign board-independent noise alongside it (`task_wdt: task not found`
  during the format window, `LittleFS Already Mounted!` warnings); the FS mounts and the engine runs.
- **Deferred to the operator / LEDGER (not blocking this freeze):** the GPIO39 side-button-hold Maintenance
  entry (`verify:maintenance` via `atom_echo_testhooks`), the physical LED colour/order eyes-on, an
  armed-deauth TX proof, and a real on-air capture (all board-independent or photographic), plus the live
  release-packaging proof of the `atom_echo` factory image through the dio guard and the web flasher (fires
  on the next tagged release). Tracked in the LEDGER alongside the other boards' deferred on-air proofs.
