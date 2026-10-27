/**
 * @file lgfx_m5stickc_plus2.h
 * @brief Explicit LovyanGFX device config for the M5StickC Plus2 panel (ADR-0002 #2, #5).
 *
 * Device-only: pulls LovyanGFX and is never compiled into the native test build. Like the Cardputer,
 * the Stick is brought up through LovyanGFX directly, not M5GFX/M5Unified, so the portable path is the
 * one exercised (ADR-0002 #5).
 *
 * The panel driver (ST7789v2), pins, offsets, inversion, and RGB order below are reused from the
 * on-device-tested `esp32-dragon-ball-radar` M5StickC Plus2 HAL (its `LGFX_M5StickC_Plus2`), confirmed
 * on the physical 135x240 panel there, not guessed from a retailer pinout (ADR-0002 §5, slice-0079).
 * As on the Cardputer, the backlight must be switched on with setBrightness() after init() or the panel
 * stays black; rotation 1 presents the panel as 240x135.
 */
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace sapper {

class LGFX_M5StickCPlus2 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_M5StickCPlus2() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = HSPI_HOST;    // dragon-ball on-device config for the Plus2
            cfg.spi_mode = 3;
            cfg.freq_write = 40000000;
            cfg.freq_read = 15000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 13;
            cfg.pin_mosi = 15;
            cfg.pin_miso = -1;
            cfg.pin_dc = 14;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 5;
            cfg.pin_rst = 12;
            cfg.pin_busy = -1;
            cfg.memory_width = 240;      // ST7789 controller memory
            cfg.memory_height = 320;
            cfg.panel_width = 135;       // physical portrait; rotation 1 presents 240x135
            cfg.panel_height = 240;
            cfg.offset_x = 52;           // dragon-ball on-device offsets for this panel
            cfg.offset_y = 40;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;           // ST7789 on this panel is inverted
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 27;
            cfg.invert = false;
            cfg.freq = 256;
            cfg.pwm_channel = 7;
            _light.config(cfg);
        }
        _panel.setLight(&_light);
        setPanel(&_panel);
    }
};

}  // namespace sapper
