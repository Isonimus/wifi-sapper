/**
 * @file main.cpp
 * @brief slice-0005 bring-up entry point: init the display HAL, draw a bring-up frame, and
 *        serve the serial control channel's observation half (ADR-0001, ADR-0002, ADR-0003).
 *
 * This is a bring-up harness, not the appliance: there is no engine yet (that is slice-3/4).
 * Its whole job is to prove the board comes up and answers over serial, so the later slices
 * build on a foundation confirmed by the verify artifact rather than assumed.
 */
#include <Arduino.h>

#include "config/active_board.h"
#include "hal/display/display_hal.h"
#include "hal/display/null_display.h"
#include "hal/serial/serial_channel.h"
#if SAPPER_BOARD_HAS_DISPLAY
#include "hal/display/lgfx_display.h"
#endif

namespace {
using namespace sapper;

/// The active display, resolved at compile time from the board's capability (ADR-0002 #4).
/// A screenless board compiles no LGFX_* class and gets the null display.
IDisplay& display() {
#if SAPPER_BOARD_HAS_DISPLAY
    static LgfxDisplay instance(kActiveBoard);
#else
    static NullDisplay instance;
#endif
    return instance;
}

// RGB565 primaries. The R/G/B blocks make a colour-order or byte-order error obvious in the
// dump artifact; the border makes an offset or clipping error obvious; the diagonal makes a
// mirror or rotation error obvious. Together they confirm the ADR-0002 §5 panel parameters.
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;

void fillRect(IDisplay& d, int x0, int y0, int w, int h, uint16_t color) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            d.drawPixel(x, y, color);
        }
    }
}

void drawBringupFrame(IDisplay& d) {
    const int w = d.width();
    const int h = d.height();
    d.fillScreen(kBlack);

    // Colour-order probe: red, green, blue blocks along the top-left.
    fillRect(d, 3, 3, 20, 20, kRed);
    fillRect(d, 25, 3, 20, 20, kGreen);
    fillRect(d, 47, 3, 20, 20, kBlue);

    // One-pixel border: any offset/clipping error clips or shifts it.
    for (int x = 0; x < w; ++x) {
        d.drawPixel(x, 0, kWhite);
        d.drawPixel(x, h - 1, kWhite);
    }
    for (int y = 0; y < h; ++y) {
        d.drawPixel(0, y, kWhite);
        d.drawPixel(w - 1, y, kWhite);
    }

    // Corner-to-corner diagonal: a mirror or rotation error flips its direction.
    const int span = w < h ? w : h;
    for (int i = 0; i < span; ++i) {
        d.drawPixel(i * (w - 1) / (span - 1), i * (h - 1) / (span - 1), kWhite);
    }
}

SerialChannel g_channel(display());

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);  // let the S3 USB-CDC endpoint enumerate before the first line

    IDisplay& d = display();
    if (!d.begin()) {
        // Fail loud: the verify script watches for [FATAL] and fails the run (ADR-0004 lane 3).
        Serial.println("[FATAL] display init failed");
    }
    drawBringupFrame(d);
    d.present();

    g_channel.announce();
}

void loop() {
    g_channel.pump();
    delay(5);
}
