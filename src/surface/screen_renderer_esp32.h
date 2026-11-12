/**
 * @file screen_renderer_esp32.h
 * @brief The device ScreenRenderer: lays a ScreenView out on the panel (ADR-0025).
 *
 * Holds the shared IDisplay (the one the bring-up frame and the serial dump use — one canvas, one
 * owner) and turns a view-model into glyphs: a persistent HUD, and a CRACKED banner over its lower
 * part when a toast is active. It is the only screen code that knows pixels, fonts, and colours, so
 * the surface policy can stay pure. Not compiled on the native lane (the fake renderer stands in
 * there); its correctness is the on-air screenshot's job (slice-0026 Scenario J), not a unit test's.
 */
#pragma once

#include "hal/display/display_hal.h"
#include "surface/screen_renderer.h"

namespace sapper {

class Esp32ScreenRenderer : public ScreenRenderer {
public:
    explicit Esp32ScreenRenderer(IDisplay& display) : display_(display) {}

    void render(const ScreenView& view) override;

private:
    /// Header band (ADR-0116 #1): the product wordmark + version. Battery, armed-TX chips draw separately.
    void drawHeader(const ScreenView& view);
    /// Status band (ADR-0116 #1/#2): the heartbeat dot + the health/phase word + the RSSI bar glyph.
    void drawStatusBand(const ScreenView& view);
    /// Footer band (ADR-0116 #1): the reference counters (this run's uploads, the lifetime cracked total)
    /// and the sync line, at the bottom so the live body is the visual centre.
    void drawFooter(const ScreenView& view);
    /// The live-hunt body (ADR-0033 / ADR-0116 #1): a SCAN line while Discovering, or the target + a
    /// Beacon/M1–M4 indicator row + a PMKID badge + a progress bar while Capturing. Hidden with no source.
    void drawHuntLine(const ScreenView& view);
    /// The CAPTURED/CRACKED banner (ADR-0031), constrained to the body region so it never covers the
    /// header/status bands or the armed-TX chips (ADR-0116 #6).
    void drawBanner(const ScreenView& view);
    /// The target signal glyph (ADR-0116 #5): four bars filled to view.huntRssi, right of the status word.
    /// Drawn only while Capturing with a known bucket (Unknown hides it; raw dBm never reaches here).
    void drawRssi(const ScreenView& view);
    /// The armed-TX header chips (ADR-0116 #6): a red `D` when deauth is armed, an amber `P` when PMKID
    /// solicitation is armed, left of the battery. Colour carries "this unit is transmitting"; hidden when passive.
    void drawArmedTx(const ScreenView& view);
    /// The battery icon (ADR-0091): an outline + fill segments from view.batteryLevel, top-right of the
    /// header row. Drawn only when view.batteryShown (a measured gauge on a calibrated board).
    void drawBattery(const ScreenView& view);
    /// Fill an axis-aligned rect via the pixel primitive — the seam has no fillRect (it grows only when
    /// needed, ADR-0025 decision 3), and render() runs only on a change, so the cost is negligible.
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color565);

    IDisplay& display_;
};

}  // namespace sapper
