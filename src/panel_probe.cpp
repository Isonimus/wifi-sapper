/**
 * @file panel_probe.cpp
 * @brief Implementation of the panel-parameter proof probe (ADR-0041). Device-only.
 */
#include "panel_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include "config/active_board.h"
#include "hal/display/display_hal.h"

namespace sapper {
namespace {

// RGB565 primaries. The R/G/B blocks make a colour-order or byte-order error obvious in the dump
// artifact; the border makes an offset or clipping error obvious; the diagonal makes a mirror or
// rotation error obvious. Together they confirm the ADR-0002 §5 panel parameters — which is why this
// frame lives on, as a bench probe, after the product splash took over the shipped boot face (ADR-0041).
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;

constexpr uint32_t kHeartbeatIntervalMs = 2000;

bool g_active = false;
uint32_t g_lastHeartbeatMs = 0;

void fillRect(IDisplay& d, int x0, int y0, int w, int h, uint16_t color) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            d.drawPixel(x, y, color);
        }
    }
}

// The panel proof itself — byte-for-byte the frame that was the shipped boot face through slice-0005.
void drawBringupFrame(IDisplay& d) {
    const int w = d.width();
    const int h = d.height();
    d.fillScreen(kBlack);

    fillRect(d, 3, 3, 20, 20, kRed);
    fillRect(d, 25, 3, 20, 20, kGreen);
    fillRect(d, 47, 3, 20, 20, kBlue);

    for (int x = 0; x < w; ++x) {
        d.drawPixel(x, 0, kWhite);
        d.drawPixel(x, h - 1, kWhite);
    }
    for (int y = 0; y < h; ++y) {
        d.drawPixel(0, y, kWhite);
        d.drawPixel(w - 1, y, kWhite);
    }

    // span is the shorter panel dimension; span >= 2 for any real panel (the smallest board profile is
    // 240x135), so the `span - 1` divisor below is never zero. Not guarded: a 1-pixel display cannot exist.
    const int span = w < h ? w : h;
    for (int i = 0; i < span; ++i) {
        d.drawPixel(i * (w - 1) / (span - 1), i * (h - 1) / (span - 1), kWhite);
    }
}

}  // namespace

bool panelProbeBegin(IDisplay& display) {
    const char* flag = SAPPER_TEST_PANEL;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    if (!kActiveBoard.hasDisplay) {
        Serial.println("[FATAL] verify:device panel proof needs a board with a panel; this board has none");
        return false;
    }
    // main.cpp already began the panel and drew the splash before dispatching probes; the display is
    // live here, so the diagnostic renders over that same canvas the serial dump streams back.
    drawBringupFrame(display);
    display.present();
    g_active = true;
    Serial.println("[PANEL] verify: bring-up frame rendered — send `dump` to capture the panel proof");
    return true;
}

bool panelProbeActive() { return g_active; }

void panelProbePump() {
    if (!g_active) return;

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[PANEL] waiting elapsed=%lums (send `dump` to capture the panel proof)\n",
                      static_cast<unsigned long>(now));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the diagnostic frame (§4 invariants #4, #22).

#include "hal/display/display_hal.h"

namespace sapper {
bool panelProbeBegin(IDisplay&) { return false; }
bool panelProbeActive() { return false; }
void panelProbePump() {}
}  // namespace sapper

#endif
