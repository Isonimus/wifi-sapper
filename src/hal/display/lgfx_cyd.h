/**
 * @file lgfx_cyd.h
 * @brief Explicit LovyanGFX device config for the ESP32-3248S035C "CYD" ST7796 panel (ADR-0002 #2/#5, ADR-0084).
 *
 * Device-only: pulls LovyanGFX, never compiled into the native test build. Like the other boards, the CYD
 * is brought up through LovyanGFX directly, not autodetect, so the portable path is the one exercised
 * (ADR-0002 #5). This panel is driven by the direct-to-panel renderer (DirectPanelDisplay, ADR-0084): there
 * is no full-frame sprite, because a 320x480 RGB565 canvas is 307200 B against this no-PSRAM board's
 * measured 110580 B largest free block (ADR-0082). The panel class here is the same shape as the others;
 * only the IDisplay wrapper differs.
 *
 * CONFIRMED on the physical panel with the panel probe on 2026-10-27 (SAPPER_TEST_PANEL, ADR-0041/0042,
 * ADR-0002 §5). The pins came from the documented ESP32-3248S035C-C pinout (consistent across ESP3D,
 * espboards.dev, an ESPHome bring-up thread, and a tested port PR; one conflicting retailer guide, for a
 * different board, was discarded), and the probe frame confirmed them all drive the panel:
 *   ST7796 (SPI, HSPI/SPI2 on the classic ESP32): CS 15, DC 2, SCK 14, MOSI 13, MISO 12, backlight 27,
 *   RST tied (-1). rgb_order=false and the offsets are clean (border reaches the edges, diagonal hits both
 * corners). rotation is set in the board profile (not here): rotation 1 presents 480x320 landscape to match
 * the other boards (slice-0085; the 1-vs-3 right-side-up direction is confirmed on the probe re-run). The
 * memory_width/height below stay the native 320x480 controller RAM regardless of rotation. The one
 * correction the probe forced:
 * `invert` was the candidate `true` but the glass is NOT inverted (true gave a whole-panel colour inversion,
 * black->white and the R/G/B blocks shown as cyan/magenta/yellow), so it is `false` below. GT911 capacitive
 * touch (I2C) is NOT configured here: it is a separate input seam with no consumer yet, deferred by
 * slice-0085 (LEDGER).
 */
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace sapper {

class LGFX_CYD : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_CYD() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;     // HSPI on the classic ESP32: the bus whose factory pins are 14/13/12
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;    // clean frame confirmed at 40 MHz (2026-10-27); 80 MHz is a later tuning option
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;        // 4-wire: MISO is wired (GPIO12), so reads do not share MOSI
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 14;            // SCK
            cfg.pin_mosi = 13;            // MOSI
            cfg.pin_miso = 12;            // MISO
            cfg.pin_dc = 2;               // DC
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 15;              // CS
            cfg.pin_rst = -1;             // RST tied on this module; confirmed fine (the panel reset and drew)
            cfg.pin_busy = -1;
            cfg.memory_width = 320;       // ST7796 controller RAM
            cfg.memory_height = 480;
            cfg.panel_width = 320;        // physical portrait glass (rotation 0 presents 320x480)
            cfg.panel_height = 480;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.readable = true;          // MISO is wired, so the controller RAM is readable
            cfg.invert = false;           // CONFIRMED on panel 2026-10-27: invert=true gave a whole-panel colour
                                          // inversion (black->white, R/G/B blocks shown as cyan/magenta/yellow)
            cfg.rgb_order = false;        // CONFIRMED on panel: blocks read red/green/blue in order, no R/B swap
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;       // the C variant's touch is I2C, so the display SPI bus is not shared
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 27;             // backlight
            cfg.invert = false;
            cfg.freq = 1200;
            cfg.pwm_channel = 7;
            _light.config(cfg);
        }
        _panel.setLight(&_light);
        setPanel(&_panel);
    }
};

}  // namespace sapper
