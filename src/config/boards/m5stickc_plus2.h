/**
 * @file m5stickc_plus2.h
 * @brief Board Profile for the M5StickC Plus2 (ESP32-PICO-V3-02, 2 MB PSRAM), the slice-8 second board.
 *
 * The first non-reference board (slice-0079) and the first PSRAM board in the tree. Pins, the ST7789v2
 * panel parameters (in `lgfx_m5stickc_plus2.h`), and the two on-device-measured gotchas below are reused
 * from the on-device-tested `esp32-dragon-ball-radar` M5StickC Plus2 HAL, not guessed (ADR-0002 §5):
 *
 *  - The battery ADC is GPIO38, not GPIO34: on the Plus2, GPIO34 read full-scale while GPIO38 reads a
 *    real cell through the board's 1:2 divider (dragon-ball measurement).
 *  - The buttons (BtnA 37, BtnB 39) are input-only pads with no internal pull-ups; the board pulls them
 *    up externally, so the glue reads them as INPUT (not INPUT_PULLUP), pressed = LOW.
 *
 * Maintenance entry is a deliberate hold of the BtnB side button (GPIO39), selected at compile time by
 * `input == InputKind::Buttons` (ADR-0078), since this board has no TCA8418 keyboard. GPIO4 is a software
 * power-latch that must be held HIGH in setup() or the Stick powers off on button release.
 *
 * Measured on device (slice-0079): with USB connected, a long-press of the power button does not fully
 * power the unit off, it enters sleep (screen off, the green power LED stays lit), and the following wake
 * does not restart the CPU cleanly until an EN reset arrives (opening a serial monitor pulses one). A true
 * power-off needs USB unplugged (battery only), where power cycling is clean and confirmed. This is
 * documented M5 board behavior below firmware (setup() never runs in the hung window), so it is a bench
 * artifact only: the appliance runs headless on battery in the field.
 */
#pragma once

#include "config/board_profile.h"

namespace sapper {

inline constexpr BoardProfile kM5StickCPlus2Profile = {
    .name = "M5StickC Plus2",
    .hasDisplay = true,
    .display = {.width = 240, .height = 135, .rotation = 1, .colorBits = 16},  // rotation 1 presents 240x135
    .input = InputKind::Buttons,
    .storage = StorageKind::LittleFs,   // no SD slot on the Stick; recovered manifest lives on flash LittleFS
    .hasPsram = true,                   // ESP32-PICO-V3-02 carries 2 MB Quad PSRAM
    .led = LedKind::Single,             // single red LED on G19 (shared with the IR emitter)
    .ledPin = 19,
    .batteryAdcPin = 38,                // measured: GPIO34 read full-scale, GPIO38 is the real divider tap
    .batteryDividerPermille = 2000,     // measured 1:2 divider (dragon-ball HAL); cell mv = pin mv x2 (ADR-0091)
    .lcdCs = 5, .lcdDc = 14, .lcdRst = 12, .lcdBl = 27,
    .sdCs = -1, .sdMosi = -1, .sdMiso = -1, .sdClk = -1,  // no SD slot
    .bootButtonPin = 39,                // BtnB side button, active-low; the Maintenance-entry hold (ADR-0078)
    .powerHoldPin = 4,                  // software power-latch: held HIGH in setup() or the device powers off
};

}  // namespace sapper
