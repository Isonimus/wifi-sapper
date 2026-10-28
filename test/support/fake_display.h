/**
 * @file fake_display.h
 * @brief An IDisplay double for the host lane that records setBrightness calls (ADR-0093).
 *
 * The inactivity dimmer drives the backlight through the IDisplay seam, so a fake that records the
 * brightness levels lets the stage->brightness policy be asserted natively, no LovyanGFX. Every draw
 * primitive is a no-op (the dimmer touches none of them); only setBrightness is observed.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "hal/display/display_hal.h"

namespace sapper_test {

class FakeDisplay : public sapper::IDisplay {
public:
    std::vector<uint8_t> brightnessCalls;  ///< Every level setBrightness was called with, in order.

    bool begin() override { return true; }
    uint16_t width() const override { return 240; }
    uint16_t height() const override { return 135; }
    void fillScreen(uint16_t) override {}
    void drawPixel(int32_t, int32_t, uint16_t) override {}
    void drawText(int32_t, int32_t, const char*, uint16_t, uint8_t) override {}
    void present() override {}
    void setBrightness(uint8_t level) override { brightnessCalls.push_back(level); }
    size_t canvasByteLength() const override { return 0; }
    size_t readCanvas(size_t, uint8_t*, size_t) const override { return 0; }

    /// The most recent brightness level (0 before any call).
    uint8_t last() const { return brightnessCalls.empty() ? 0 : brightnessCalls.back(); }
};

}  // namespace sapper_test
