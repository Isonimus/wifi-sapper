/**
 * @file atom_echo.h
 * @brief Board Profile for the M5Stack Atom Echo (ESP32-PICO-D4, no PSRAM, 4 MB), the slice-0105 board.
 *
 * The first board that records no new architectural decision: its capability vector is the intersection of
 * boards already in the tree, so every axis reuses an existing ADR (slice-0105).
 *
 *  - Headless. The Atom Echo has no panel at all, so `hasDisplay = false` and no `SAPPER_BOARD_HAS_DISPLAY`
 *    flag is set in its env: the HAL resolves to `NullDisplay` and no `LGFX_*` class compiles (ADR-0002 #4,
 *    ADR-0045). It is the first board for which that guard ships *permanently* absent (the CYD exercised the
 *    path transiently at slice-0083, then slice-0085 gave it a panel). The display geometry below is the
 *    meaningless-when-headless zero (the struct documents `display` as read only when `hasDisplay`).
 *  - No PSRAM + classic ESP32, the CYD's case: the two ~28 KB static `CrackedManifest` instances overflow
 *    `.dram0.bss` with no PSRAM to escape to, so the env sets `-DSAPPER_MANIFEST_HEAP_INTERNAL` and the one
 *    live manifest is placed in the internal DRAM heap (ADR-0082). `hasPsram = false`.
 *  - Single SK6812 RGB LED on GPIO27 (GRB), read from the board's own ESPHome `esp32_rmt_led_strip` log. It
 *    is the same one-wire addressable protocol and byte order as the Cardputer's WS2812, so `LedKind::Rgb`
 *    on pin 27 reuses the `led_driver_esp32.cpp` Rgb path unchanged. The LED is this board's primary local
 *    surface (it is otherwise headless), so colour fidelity/order is settled eyes-on at bring-up.
 *  - Maintenance entry is a deliberate hold of the side button on GPIO39, selected at compile time by
 *    `input == InputKind::Buttons` (ADR-0078). This is the *same pin and electrical profile* as the
 *    M5StickC Plus2's BtnB: an input-only pad with no internal pull, pulled up externally by the board, so
 *    the glue reads it as INPUT (not INPUT_PULLUP), pressed = LOW.
 *  - No battery (USB/Grove powered): `batteryAdcPin = -1`, `batteryDividerPermille = 0` -> Unknown (ADR-0091).
 *  - No software power-latch (unlike the Stick's GPIO4): `powerHoldPin = -1`, like the USB-powered
 *    Cardputer/CYD. That `setup()` runs to completion without a latch is confirmed at on-device bring-up.
 *  - No SD slot: captures/config persist on flash LittleFS (`StorageKind::LittleFs`), as on the Stick/CYD.
 *
 * The I2S microphone, speaker, and IR of the voice-assistant hardware have no consumer in the Sapper, so the
 * profile models only what the engine and its surfaces use (ADR-0002) and declares no fields for them.
 */
#pragma once

#include "config/board_profile.h"

namespace sapper {

inline constexpr BoardProfile kAtomEchoProfile = {
    .name = "M5Stack Atom Echo",
    .hasDisplay = false,
    .display = {.width = 0, .height = 0, .rotation = 0, .colorBits = 0},  // headless: meaningless, zeroed
    .input = InputKind::Buttons,
    .storage = StorageKind::LittleFs,   // no SD slot; recovered manifest lives on flash LittleFS
    .hasPsram = false,                  // ESP32-PICO-D4 carries no PSRAM (confirmed: device log "PSRAM: NO")
    .led = LedKind::Rgb,                // single SK6812 on G27, GRB (same one-wire family as the Cardputer)
    .ledPin = 27,
    .batteryAdcPin = -1,                // no battery (USB/Grove powered)
    .batteryDividerPermille = 0,        // no divider to measure -> BatteryLevel::Unknown (ADR-0091)
    .lcdCs = -1, .lcdDc = -1, .lcdRst = -1, .lcdBl = -1,  // headless: no panel
    .sdCs = -1, .sdMosi = -1, .sdMiso = -1, .sdClk = -1,  // no SD slot
    .bootButtonPin = 39,                // side button, active-low; the Maintenance-entry hold (ADR-0078)
    .powerHoldPin = -1,                 // no software power-latch (USB-powered, like the Cardputer/CYD)
};

}  // namespace sapper
