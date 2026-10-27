---
id: '0083'
title: "ESP32-3248S035C (CYD) headless hunter bring-up"
type: slice
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Goal

Bring up the ESP32-3248S035C ("CYD") as the fourth supported board, as a **headless hunter**: the
capture -> upload -> sync engine, the Maintenance phase, and provisioning, all running on a classic
ESP32-D0WD-V3 with no PSRAM and 4 MB flash, with no display this slice. This is the architecturally hard
board, the first that is **both no-PSRAM and classic ESP32**, so it is the one that forced a new
memory-placement decision (ADR-0082): the two ~28 KB static `CrackedManifest` instances overflow the
classic-ESP32 `.dram0.bss` budget and there is no PSRAM to escape to. The headless-first core (ADR-0001)
is board-independent, so a working hunter ships now; lighting the 320x480 ST7796 panel is a separate,
additive Slice B that needs a canvas-free renderer (the full-frame-sprite HAL would need a 300 KB
`createSprite` block, infeasible on this board).

## Definition of Done

- **Given** the `cyd_3248s035c` env built on ADR-0080's no-PSRAM static `.bss` path
- **When** `pio run -e cyd_3248s035c` links
- **Then** it **fails** with `.dram0.bss will not fit in region dram0_0_seg` / `overflowed by N bytes` —
  the measured problem that justifies ADR-0082 (N recorded in `## As built`).

- **Given** the `cyd_3248s035c` env with `-DSAPPER_MANIFEST_HEAP_INTERNAL` (ADR-0082)
- **When** `pio run -e cyd_3248s035c` links
- **Then** it compiles and links clean (no `.dram0.bss` overflow), because both static manifests leave
  `.bss` and the one live manifest is placed in the internal DRAM heap at runtime; the RAM/Flash figures
  are recorded in `## As built`.

- **Given** a flashed `cyd_3248s035c` build on an unprovisioned device
- **When** it boots
- **Then** the serial log reaches `[STATE] phase=provisioning` over the CH340 bridge with `disp=0x0` (the
  headless NullDisplay), **no `[FATAL] display init failed`** line (a screenless board is the designed
  state, ADR-0045), and the captive portal comes up; the measured `heap_max` (largest free block) is
  recorded in `## As built` and is >> 28 KB, confirming the internal-heap manifest block is available.

- **Given** a provisioned `cyd_3248s035c`
- **When** it associates and both manifest callers run (the `hunt_loop` mirror and, after a Maintenance
  entry, the `runMaintenance` mirror)
- **Then** each manifest is allocated from the internal DRAM heap with no `[FATAL] ... internal-heap
  allocation failed`, and the hunt and the Maintenance dashboard read the cracked mirror correctly
  (the full manifest-from-internal-heap proof, the operator's to run, LEDGER).

- **Given** a provisioned `cyd_3248s035c`
- **When** the operator holds the BOOT button (GPIO0) for ~2 s during the hunt
- **Then** `decideButtonHold` fires once, the device persists the reboot-intent and restarts into
  Maintenance, and the Maintenance SoftAP serves the dashboard (§4 #21, ADR-0078); confirmed end-to-end
  by `verify:maintenance` against the flashed board.

- **Given** a flashed `cyd_3248s035c` with real credentials
- **When** it associates and runs the endless hunt
- **Then** the capture -> upload -> sync engine runs as on the reference board (the headless-first core is
  board-independent, ADR-0001), observed over serial.

## Design

**New decision, measured first.** The only genuinely new work is ADR-0082 (internal-heap manifest
placement on a no-PSRAM classic ESP32). Its number was measured before the decision: the `cyd_3248s035c`
env built on ADR-0080's static path overflowed `dram0_0_seg` by **27056 bytes**, and the device's measured
largest free block at boot is 110580 B, so a 28 KB runtime alloc fits with ~4x margin. See ADR-0082.

**Reused, not rebuilt.**

- **Manifest placement (ADR-0080 -> ADR-0082).** The placement is a board-conditional pointer +
  placement-new at the same two sites ADR-0080 already uses (`hunt_loop.cpp`, `main.cpp`). ADR-0082 adds
  one `#elif defined(SAPPER_MANIFEST_HEAP_INTERNAL)` branch (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`) and a
  matching `[FATAL]` guard; the pure `CrackedManifest` policy and its host tests are untouched (placement
  is the caller's concern), so the native suite is unchanged by construction.
- **Entry gesture (ADR-0078).** `InputKind::Buttons` with `bootButtonPin = 0` (the BOOT button, the same
  pattern as the T-embed's encoder push on GPIO0). The device glue in `main.cpp` already does the
  `pinMode(INPUT)` + `digitalRead == LOW` + `decideButtonHold(...)` for any Buttons board, and
  `maintenanceEntryRequested()` is already selected by `InputKind`. No new pure code, so
  `test/test_button_hold` already covers the decision; the GPIO0 electrical read is the only on-device
  unknown.
- **Headless display path (ADR-0002 #4, ADR-0045).** `hasDisplay = false` + no `SAPPER_BOARD_HAS_DISPLAY`
  resolves the HAL to `NullDisplay`, whose `begin()` returns true (the designed screenless state, not a
  fault). `hunt_loop` already skips the screen surface when `!kActiveBoard.hasDisplay`. The one gap closed
  this slice: `lgfx_display.h/.cpp` compiled unconditionally in every device build and demanded a panel
  config, so a headless device env failed to compile. Both are now guarded by `SAPPER_BOARD_HAS_DISPLAY`,
  making the "a screenless board compiles no LGFX_* class" contract true at the file level; a board that
  declares a display but forgets its panel config still hits the hard `#error`.
- **Board + flash.** `board = esp32dev` (classic ESP32), `flash_size = 4MB`, `partitions = huge_app.csv`
  (the standard no-OTA 4 MB layout: 3 MB app + ~896 KB spiffs/data). The stock esp32dev layout's 1.25 MB
  app slot overflows the ~1.4 MB firmware (the same note the Stick's env carries); huge_app gives a 3 MB
  app slot the firmware fits with margin, and LittleFS mounts on the spiffs-subtype data partition exactly
  as on the other boards. CH340 UART bridge (resets on open), so no `ARDUINO_USB_*` flags (unlike the two
  native-USB S3 boards).
- **Testhooks.** A `cyd_3248s035c_testhooks` env extends `cyd_3248s035c` and pulls the shared
  `[testhooks]` section, so its probe-macro set cannot drift from the other three boards (the DRY fix
  slice-0079 introduced).

**Considered and deliberately not reused / not added:**

- **No display / touch this slice.** The ST7796 320x480 panel's full-frame-sprite HAL would allocate a
  300 KB `createSprite` block (320x480x2), infeasible on a no-PSRAM classic ESP32 whose largest free block
  is ~110 KB. Lighting this panel needs a canvas-free/banded renderer, a new ADR and Slice B; the GT911
  capacitive touch comes with it. `hasDisplay = false` keeps the slice honest (no half-working panel) and
  the panel pins are left `-1` rather than guessed from a retailer pinout before the panel probe confirms
  them (ADR-0002 §5).
- **No `InputKind::Touch`.** Touch needs the panel/I2C that Slice B brings up; Maintenance entry needs only
  the BOOT button, which `InputKind::Buttons` already covers. A touch entry path for a headless board would
  be speculative.
- **No RGB LED.** The CYD's status LED is a 3-GPIO common-anode RGB (active-low), which the single-`ledPin`
  `LedKind` model does not express (the same reason the T-embed's 2-wire APA102 is deferred). `led = None`
  until a slice gives it a job (LEDGER).
- **No SD.** The TF slot is present but stores live on flash LittleFS as on the Stick/T-embed; SD is a
  future slice (LEDGER).
- **No smaller `kMaxCrackedEntries` (the rejected fork).** Capping the manifest per-board to fit static
  `.bss` would cut the CYD's cracked-AP capacity on an endlessly-hunting appliance and make a pure-core
  constant board-dependent. ADR-0082's internal-heap route keeps the full 256 and leaves the pure policy
  untouched, so it was chosen over the cap.

## Verification

- **`pio run -e cyd_3248s035c`** — the device link is the machine check for the manifest fix (§4 #37, like
  #36 on the Stick): the static-`.bss` path is a hard build failure (overflow), and the
  `-DSAPPER_MANIFEST_HEAP_INTERNAL` path links. Both outcomes are recorded in `## As built`.
- **`test/test_button_hold`** (ADR-0078) — the pure Maintenance-entry hold decision, already covering the
  `InputKind::Buttons` GPIO0 gesture this board reuses; no new pure code, so no new test.
- **The existing manifest suites** (`test/test_cracked_manifest`, `test/test_submitted_manifest`) are
  unchanged: placement is the device caller's concern, so the pure policy they cover is byte-for-byte.
- **On-device headless boot** (not unit-assertable): the flashed build reaches `[STATE] phase=provisioning`
  with `disp=0x0`, no display `[FATAL]`, and a measured `heap_max` >> 28 KB (captured in `## As built`).
- **`verify:maintenance`** against the flashed, provisioned board confirms the GPIO0 hold -> reboot ->
  Maintenance SoftAP path end-to-end (ADR-0078/§4 #21); the operator's to run, like the Stick/T-embed.
- The full **manifest-from-internal-heap** hunt + Maintenance read needs a provisioned device (real WiFi +
  a wpa-sec account) and is the operator's to run, tracked in the LEDGER alongside the Stick's deferred
  PSRAM boot proof.

## As built

Shipped as designed: the CYD boots and runs headless on the internal-heap manifest placement.

- **Static-path overflow (the measured problem):** building `cyd_3248s035c` on ADR-0080's static `.bss`
  path failed at link with `region dram0_0_seg overflowed by 27056 bytes` (vs the Stick's 28816; smaller
  because the headless CYD compiles no display `.bss`).
- **Fork-B link (the fix):** with `-DSAPPER_MANIFEST_HEAP_INTERNAL` the env links clean — **RAM 28.8%**
  (94248 B of 327680), **Flash 42.1%** (1323467 B of the 3 MB app slot). The overflow is gone; the link is
  the machine check (§4 #37).
- **On-device headless boot:** flashed over CH340 (`/dev/ttyUSB0`), the board boots (classic-ESP32 ROM, DIO
  flash) to `[STATE] phase=provisioning heap_free=245888 heap_max=110580 disp=0x0`, with no
  `[FATAL] display init failed` line, and the captive portal comes up (`Sapper-XXXX`, the SoftAP name
  redacted per §4 #25). `disp=0x0` confirms the headless NullDisplay; `heap_max=110580` (largest free
  block) is ~4x the 28 KB the manifest needs, so the internal-heap alloc has ample room.
- **Provisioned on-device run (2026-10-27):** the operator provisioned the board; serial confirmed the
  hunt runs end-to-end on the internal-heap manifest. The device associated, ran the engine
  (`[DEAUTH] txOk=2→4 capturing=1`: a target selected, captured, and deauth transmitted), and fired a sync
  (`[SYNC] outcome ... fetch=1`) — all of which require the internal-heap manifest alloc to have succeeded
  (no `[FATAL] ... internal-heap allocation failed`). This proves ADR-0082 on device (the manifest alloc +
  the hunt reading/writing it). **One leg failed:** the wpa-sec cracked-download TLS handshake errored
  (`ssl_starttls_handshake -80`, "connection refused"; `[SYNC] ok=0`), so the download+seed leg of the
  "hunt→upload→sync runs as on the reference board" DoD scenario is NOT yet confirmed on this board. It is
  board-independent sync-path code (ADR-0001), not the manifest placement this slice decides; its root cause
  (first-boot sync before NTP set the clock for cert-time validation, vs no-PSRAM-classic TLS heap pressure
  possibly aggravated by the 28 KB internal-heap manifest) is tracked in the LEDGER.
- **Deferred to the operator / LEDGER:** the wpa-sec TLS download root-cause (above), the Maintenance
  dashboard read-from-internal-heap + the GPIO0 hold `verify:maintenance` run, and Slice B (ST7796 320x480
  panel + GT911 touch via a canvas-free renderer).
