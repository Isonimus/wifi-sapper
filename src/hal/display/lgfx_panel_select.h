/**
 * @file lgfx_panel_select.h
 * @brief Compile-time selection of the active board's LovyanGFX panel class (ADR-0002 #2, ADR-0084).
 *
 * Device-only: pulls LovyanGFX, so it is guarded by SAPPER_BOARD_HAS_DISPLAY and compiles to nothing on a
 * screenless board (ADR-0002 #4). Both IDisplay backends need the panel type: the sprite-backed LgfxDisplay
 * (lgfx_display.h) and the buffer-free DirectPanelDisplay (direct_panel_display.h, ADR-0084). The selection
 * lives here, in one place, so the two backends share it rather than each carrying a copy (DRY, two
 * consumers). A board that declares a display but forgets its panel config still hits the hard #error.
 */
#pragma once

#include "config/active_board.h"   // SAPPER_BOARD_HAS_DISPLAY: a screenless board compiles no LGFX_* class

#if SAPPER_BOARD_HAS_DISPLAY

// The active board's LovyanGFX config class is selected by the same SAPPER_BOARD_* flag as its profile
// (ADR-0002 #2). This selection is device-only (it pulls LovyanGFX), so it is not visible on the native lane.
#if defined(SAPPER_BOARD_CARDPUTER)
#include "hal/display/lgfx_cardputer.h"
namespace sapper {
using ActiveLgfxPanel = LGFX_Cardputer;
}
#elif defined(SAPPER_BOARD_M5STICKC_PLUS2)
#include "hal/display/lgfx_m5stickc_plus2.h"
namespace sapper {
using ActiveLgfxPanel = LGFX_M5StickCPlus2;
}
#elif defined(SAPPER_BOARD_T_EMBED)
#include "hal/display/lgfx_t_embed.h"
namespace sapper {
using ActiveLgfxPanel = LGFX_T_embed;
}
#elif defined(SAPPER_BOARD_ESP32_3248S035C)
#include "hal/display/lgfx_cyd.h"
namespace sapper {
using ActiveLgfxPanel = LGFX_CYD;
}
#else
#error "No SAPPER_BOARD_* panel config selected — define one in platformio.ini build_flags"
#endif

#endif  // SAPPER_BOARD_HAS_DISPLAY
