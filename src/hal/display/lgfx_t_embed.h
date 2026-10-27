/**
 * @file lgfx_t_embed.h
 * @brief Explicit LovyanGFX device config for the LilyGo T-embed panel (ADR-0002 #2, #5).
 *
 * Device-only: pulls LovyanGFX and is never compiled into the native test build. Like the Cardputer and
 * the Stick, the T-embed is brought up through LovyanGFX directly, not M5GFX/autodetect, so the portable
 * path is the one exercised (ADR-0002 #5).
 *
 * The SPI and control GPIOs are LilyGo's on-device-tested factory values (pin_config.h): SCLK 12, MOSI 11,
 * DC 13, CS 10, RST 9, BL 15. The ST7789 1.9" 170x320 panel parameters (memory window, offsets, inversion,
 * RGB order) are seeded from the closest tested sibling, the LilyGo T-display-s3 (same controller and glass
 * family): a 170-wide window centred in the 240-wide controller RAM gives offset_x = (240-170)/2 = 35. These
 * are the panel-parameter quantities ADR-0002 §5 requires measuring on the real panel, so they are
 * CONFIRMED/TUNED with the panel probe (SAPPER_TEST_PANEL -> verify:device, ADR-0041/0042); adjust the
 * offsets/invert/rgb_order here if the proof frame clips or mis-colours. The panel profile uses rotation 3
 * to present 320x170 upright (rotation 1 was measured 180-flipped on this unit). As on the other boards the
 * backlight must be switched on with setBrightness() after init() or the panel stays black.
 */
#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace sapper {

class LGFX_T_embed : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX_T_embed() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI2_HOST;    // FSPI on the S3
            cfg.spi_mode = 0;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = true;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 12;           // PIN_LCD_CLK (LilyGo factory pin_config)
            cfg.pin_mosi = 11;           // PIN_LCD_MOSI
            cfg.pin_miso = -1;
            cfg.pin_dc = 13;             // PIN_LCD_DC
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 10;             // PIN_LCD_CS
            cfg.pin_rst = 9;             // PIN_LCD_RES
            cfg.pin_busy = -1;
            cfg.memory_width = 240;      // ST7789 controller RAM
            cfg.memory_height = 320;
            cfg.panel_width = 170;       // physical portrait glass; rotation 1 presents 320x170
            cfg.panel_height = 320;
            cfg.offset_x = 35;           // (240-170)/2, centred window; confirmed on panel via the panel probe
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            cfg.readable = false;
            cfg.invert = true;           // ST7789 on this glass is inverted (sibling T-display-s3)
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 15;             // PIN_LCD_BL
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
