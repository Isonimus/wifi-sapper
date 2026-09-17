/**
 * @file cardputer.h
 * @brief Board Profile for the M5Stack Cardputer ADV (ESP32-S3, no PSRAM) — reference board.
 *
 * The physical reference device is the **Cardputer ADV**. The Adversary repo labels this
 * hardware as plain "Cardputer" throughout — everything it calls Cardputer is in fact the
 * ADV — so its `m5stack-stamps3` board id, pins, and display config are the ADV's and are
 * reused here directly.
 *
 * Pin values are the Adversary's measured pins (`src/config/pins.h`). The display geometry is
 * the 1.14" 240x135 panel; the exact LovyanGFX panel/offset parameters live in the
 * `LGFX_Cardputer` config class and are confirmed by slice-0005's bring-up artifact on the
 * actual ADV, not asserted here (ADR-0002 §5).
 */
#pragma once

#include "config/board_profile.h"

namespace sapper {

// RGB565 canvas (16bpp) for bring-up. The memory ADR (deferred, with the upload slice) may
// drop this to an 8bpp palette on this no-PSRAM board; colourBits is a profile field so that
// tuning is a data change, not a code change (ADR-0002 #3).
inline constexpr BoardProfile kCardputerProfile = {
    .name = "M5Stack Cardputer ADV",
    .hasDisplay = true,
    .display = {.width = 240, .height = 135, .rotation = 1, .colorBits = 16},
    .input = InputKind::Keyboard,
    .storage = StorageKind::Sd,
    .hasPsram = false,
    .led = LedKind::Rgb,   // single WS2812 on G21
    .ledPin = 21,
    .batteryAdcPin = 10,
    .lcdCs = 37, .lcdDc = 34, .lcdRst = 33, .lcdBl = 38,
    .sdCs = 12, .sdMosi = 14, .sdMiso = 39, .sdClk = 40,
};

}  // namespace sapper
