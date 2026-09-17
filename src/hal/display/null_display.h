/**
 * @file null_display.h
 * @brief No-op IDisplay for screenless boards (ADR-0002 #4).
 *
 * Pure C++ — no LovyanGFX, no Arduino — so it compiles and is unit-tested natively. On a
 * board with `hasDisplay = false` this is what the display HAL resolves to: every draw is a
 * no-op, geometry is zero, and `dumpCanvas` yields nothing. `begin()` returns true because
 * the absence of a panel is the designed state, not an error.
 */
#pragma once

#include "hal/display/display_hal.h"

namespace sapper {

class NullDisplay final : public IDisplay {
public:
    bool begin() override { return true; }
    uint16_t width() const override { return 0; }
    uint16_t height() const override { return 0; }
    void fillScreen(uint16_t /*color565*/) override {}
    void drawPixel(int32_t /*x*/, int32_t /*y*/, uint16_t /*color565*/) override {}
    void present() override {}
    size_t canvasByteLength() const override { return 0; }
    size_t readCanvas(size_t /*offset*/, uint8_t* /*out*/, size_t /*cap*/) const override {
        return 0;
    }
};

}  // namespace sapper
