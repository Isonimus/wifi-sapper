/**
 * @file lgfx_cardputer.h
 * @brief Explicit LovyanGFX device config for the Cardputer panel (ADR-0002 #2, #5).
 *
 * Device-only: pulls LovyanGFX and is never compiled into the native test build. The
 * Cardputer is brought up through LovyanGFX directly, not M5GFX, so the portable path is
 * exercised from day one (ADR-0002 #5).
 *
 * ⚠ The panel driver, offsets, inversion, and RGB order below are the community-known
 * starting hypothesis for the 240x135 Cardputer panel. ADR-0002 §5 deliberately does NOT
 * assert them here — slice-0005's `dump` artifact (a PNG reconstructed from the canvas) is
 * what confirms or corrects them. If the bring-up image is offset, mirrored, or colour-
 * swapped, the fix is here and the corrected values are recorded in the slice's `As built`.
 */
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace sapper {

class LGFX_Cardputer : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_Cardputer() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;   // ESP32-S3 FSPI
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 36;          // display SCLK (not in the Adversary pins.h — M5GFX held it)
            cfg.pin_mosi = 35;          // display MOSI
            cfg.pin_miso = -1;
            cfg.pin_dc = 34;            // LCD_DC (pins.h)
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 37;            // LCD_CS (pins.h)
            cfg.pin_rst = 33;           // LCD_RST (pins.h)
            cfg.pin_busy = -1;
            cfg.panel_width = 135;      // physical portrait; rotation 1 presents 240x135
            cfg.panel_height = 240;
            cfg.offset_x = 52;          // ⚠ hypothesis — confirmed by the dump artifact
            cfg.offset_y = 40;          // ⚠ hypothesis
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;          // ⚠ hypothesis
            cfg.rgb_order = false;      // ⚠ hypothesis
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 38;            // LCD_BL (pins.h)
            cfg.invert = false;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
        }
        _panel.setLight(&_light);
        setPanel(&_panel);
    }
};

}  // namespace sapper
