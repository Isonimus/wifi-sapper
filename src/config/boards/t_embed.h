/**
 * @file t_embed.h
 * @brief Board Profile for the LilyGo T-embed (ESP32-S3, 8 MB octal PSRAM, 16 MB flash), slice-0081.
 *
 * The third board (after the reference Cardputer and the M5StickC Plus2) and the first octal-PSRAM S3.
 * It introduces no new mechanism: Maintenance entry reuses ADR-0078 (`InputKind::Buttons` +
 * `decideButtonHold`) and the large-manifest placement reuses ADR-0080 (`BOARD_HAS_PSRAM` heap route,
 * or the Cardputer's static-DRAM path when PSRAM is absent).
 *
 * Pins are taken from LilyGo's on-device-tested factory firmware (`examples/factory/pin_config.h`), not
 * guessed from a retailer pinout (ADR-0002 §5). The ST7789 panel parameters live in `lgfx_t_embed.h` and
 * are confirmed on the physical panel with the panel probe (ADR-0041/0042).
 *
 * GPIO0 note (the one a later operator will try to "fix"): `bootButtonPin` is deliberately 0, the encoder
 * push switch. On the T-embed GPIO0 is wired to that switch in the standard ESP32 BOOT-button pattern:
 * an external strap pull-up holds it HIGH, pressing pulls it LOW, and it is freely readable at runtime.
 * This is NOT the Cardputer ADV case (ADR-0057), where GPIO0 is a bare boot strap with no runtime-pressable
 * button, so it declares -1 and enters Maintenance by keyboard instead. Here a runtime read of GPIO0 is
 * correct; the strap role only matters at reset (holding the encoder across an EN reset enters ROM download
 * mode, which is a flashing convenience, not a runtime hazard). Do not "restore" this to -1 to match the
 * board_profile.h default-member warning: that warning guards an *accidental* zero, and this zero is the
 * measured, deliberate entry button (ADR-0078).
 *
 * Deferred on this board (recorded here as data for a future slice, not wired in slice-0081):
 *  - Rotary encoder rotation lines A=GPIO2, B=GPIO1 (unused: the HUD is read-only and Maintenance is
 *    served over the SoftAP, so only the push switch has a job today).
 *  - APA102 status LED: CLK=GPIO45, DATA=GPIO42 (a 2-wire LED the single-`ledPin` model does not cover;
 *    `led = None` until a slice wires it, LEDGER).
 *  - MicroSD slot: CS=GPIO39, SCK=GPIO40, MOSI=GPIO41, MISO=GPIO38 (stores live on flash LittleFS as on
 *    the Stick; SD is a future slice, LEDGER).
 */
#pragma once

#include "config/board_profile.h"

namespace sapper {

inline constexpr BoardProfile kTEmbedProfile = {
    .name = "LilyGo T-embed",
    .hasDisplay = true,
    .display = {.width = 320, .height = 170, .rotation = 3, .colorBits = 16},  // rotation 3 presents 320x170 upright (rotation 1 was 180-flipped on this panel, measured on device)
    .input = InputKind::Buttons,        // the encoder push switch, read as a hold (ADR-0078)
    .storage = StorageKind::LittleFs,   // recovered manifest + stores live on flash LittleFS (no SD wired yet)
    .hasPsram = true,                   // 8 MB octal PSRAM; confirmed live on device by the heap-caps proof (ADR-0080)
    .led = LedKind::None,               // APA102 (2-wire) deferred; single-ledPin model does not cover it
    .ledPin = -1,
    .batteryAdcPin = 4,                 // PIN_BAT_VOLT (LilyGo factory pin_config)
    .lcdCs = 10, .lcdDc = 13, .lcdRst = 9, .lcdBl = 15,
    .sdCs = -1, .sdMosi = -1, .sdMiso = -1, .sdClk = -1,  // SD slot present on the board, not wired this slice
    .bootButtonPin = 0,                 // encoder push switch (GPIO0), active-low; the Maintenance-entry hold (ADR-0078)
    .powerHoldPin = 46,                 // PIN_POWER_ON: held HIGH in setup() or the board powers off (like the Stick's GPIO4)
};

}  // namespace sapper
