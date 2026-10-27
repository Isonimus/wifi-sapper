/**
 * @file esp32_3248s035c.h
 * @brief Board Profile for the ESP32-3248S035C "CYD" (classic ESP32-D0WD-V3, no PSRAM, 4 MB flash), slice-0083.
 *
 * The fourth board and the first that is BOTH no-PSRAM AND a classic ESP32 (the Cardputer is no-PSRAM
 * but a roomy S3; the Stick is classic but has PSRAM). That pair is the architecturally hard case: the
 * two ~28 KB CrackedManifest instances overflow the classic-ESP32 static-DRAM budget (measured: the Stick
 * overflowed `.dram0.bss` by 28816 bytes with both static, ADR-0080) and there is no PSRAM to escape to.
 * ADR-0082 resolves it: on a no-PSRAM classic ESP32 the live manifest is placed in the internal DRAM heap
 * one-at-a-time (the hunt and Maintenance phases are mutually exclusive, so only one is ever live), which
 * the `cyd_3248s035c` env selects with `-DSAPPER_MANIFEST_HEAP_INTERNAL`.
 *
 * slice-0083 is the HEADLESS hunter: `hasDisplay = false`, so no `LGFX_*` class is compiled and the HAL
 * resolves to the NullDisplay (ADR-0002 #4). This deliberately defers the ST7796 320x480 panel to a later
 * slice: the full-frame-sprite display HAL would try to `createSprite(320, 480)` = 300 KB in one heap block,
 * infeasible on a no-PSRAM classic ESP32, so lighting this panel needs a canvas-free/banded renderer (a new
 * ADR, Slice B). Headless-first is exactly what the appliance is built for (CLAUDE.md), so a working hunter
 * ships now and the panel is a separate, additive slice. `hasDisplay = false` also means no spurious
 * `[FATAL] display init failed` (ADR-0045): the NullDisplay's begin() returns true, the designed screenless
 * state, not an error.
 *
 * Maintenance entry reuses ADR-0078 (`InputKind::Buttons` + `decideButtonHold`) on GPIO0, the BOOT button.
 * GPIO0 note (the one a later operator will try to "fix"): `bootButtonPin` is deliberately 0. On this board
 * GPIO0 is wired to the BOOT button in the standard ESP32 pattern: an external strap pull-up holds it HIGH,
 * pressing pulls it LOW, and it is freely readable at runtime. This is NOT the Cardputer ADV case (ADR-0057),
 * where GPIO0 is a bare boot strap with no runtime-pressable button (so the ADV declares -1 and enters
 * Maintenance by keyboard). Here a runtime read of GPIO0 is correct, exactly as on the T-embed's encoder
 * push (also GPIO0). The strap role only matters at reset (holding BOOT across an EN reset enters ROM
 * download mode, a flashing convenience, not a runtime hazard). Do not "restore" this to -1 to match the
 * board_profile.h default-member warning: that warning guards an *accidental* zero, and this zero is the
 * deliberate entry button (ADR-0078).
 *
 * USB is a CH340 UART bridge on /dev/ttyUSB (resets on port open, like the Stick's CH9102), NOT a native
 * USB-CDC like the two S3 boards, so the env sets no ARDUINO_USB_* flags.
 *
 * Deferred on this board (recorded here as data for later slices, not wired in slice-0083):
 *  - ST7796 3.5" 320x480 SPI panel + GT911 capacitive touch (I2C): the display + touch bring-up is Slice B,
 *    which needs the canvas-free renderer ADR before any pin is confirmed on the physical panel (ADR-0041/0042).
 *    Pins are therefore left -1 here rather than guessed from a retailer pinout (ADR-0002 §5).
 *  - RGB status LED: three separate common-anode GPIOs (active-low), which the single-`ledPin` LedKind model
 *    does not cover (the same reason the T-embed's 2-wire APA102 is deferred); `led = None` until a slice
 *    wires it (LEDGER).
 *  - MicroSD (TF) slot: present on the board; stores live on flash LittleFS as on the Stick/T-embed, so SD is
 *    a future slice (LEDGER).
 */
#pragma once

#include "config/board_profile.h"

namespace sapper {

inline constexpr BoardProfile kEsp32_3248S035CProfile = {
    .name = "ESP32-3248S035C (CYD)",
    .hasDisplay = false,                // headless hunter this slice; ST7796 320x480 panel is Slice B (ADR-0082 context)
    .display = {.width = 0, .height = 0, .rotation = 0, .colorBits = 0},  // meaningful only when hasDisplay is true
    .input = InputKind::Buttons,        // GPIO0 BOOT button, read as a hold (ADR-0078), like the T-embed's encoder push
    .storage = StorageKind::LittleFs,   // recovered manifest + stores live on flash LittleFS (no SD wired yet)
    .hasPsram = false,                  // classic ESP32-D0WD-V3, no PSRAM (confirmed on device by esptool Features)
    .led = LedKind::None,               // 3-GPIO common-anode RGB; single-ledPin model does not cover it (deferred)
    .ledPin = -1,
    .batteryAdcPin = -1,                // USB-powered dev board, no battery gauge
    .lcdCs = -1, .lcdDc = -1, .lcdRst = -1, .lcdBl = -1,   // ST7796 panel pins confirmed in Slice B via the panel probe
    .sdCs = -1, .sdMosi = -1, .sdMiso = -1, .sdClk = -1,   // TF slot present on the board, not wired this slice
    .bootButtonPin = 0,                 // BOOT button (GPIO0), active-low; the Maintenance-entry hold (ADR-0078)
    .powerHoldPin = -1,                 // USB-powered, no software power latch (unlike the Stick's GPIO4)
};

}  // namespace sapper
