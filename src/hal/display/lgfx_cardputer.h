/**
 * @file lgfx_cardputer.h
 * @brief Explicit LovyanGFX device config for the Cardputer panel (ADR-0002 #2, #5).
 *
 * Device-only: pulls LovyanGFX and is never compiled into the native test build. The
 * Cardputer is brought up through LovyanGFX directly, not M5GFX, so the portable path is
 * exercised from day one (ADR-0002 #5).
 *
 * The panel driver, pins, offsets, inversion, and RGB order below are taken from M5GFX's
 * `board_M5CardputerADV` configuration (the proven config for this exact hardware) and were
 * confirmed on the physical panel in slice-0005's bring-up (ADR-0002 §5). The `dump` artifact
 * proves the render pipeline; the screen itself confirmed these panel parameters. Note the
 * SPI bus is SPI3_HOST and the backlight must be switched on with setBrightness() after
 * init() — a panel that inits fine still shows black until then.
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
            cfg.spi_host = SPI3_HOST;   // matches M5GFX's proven Cardputer ADV config
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
            cfg.offset_x = 52;          // M5GFX board_M5CardputerADV; confirmed on hardware
            cfg.offset_y = 40;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;          // ST7789 on this panel is inverted
            cfg.rgb_order = false;
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
