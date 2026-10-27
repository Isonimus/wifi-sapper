/**
 * @file active_board.h
 * @brief Resolves the one `SAPPER_BOARD_*` build flag to the active Board Profile (ADR-0002).
 *
 * Exactly one board is selected per build via a single flag in `platformio.ini`. This header
 * maps that flag to `kActiveBoard` and to the `SAPPER_BOARD_HAS_DISPLAY` guard that decides
 * whether any `LGFX_*` class is compiled at all (ADR-0002 #4). A build with no board flag is
 * a hard error, not a silent default — the wrong board must fail loud at compile time.
 */
#pragma once

#include "config/board_profile.h"

#if defined(SAPPER_BOARD_CARDPUTER)
#include "config/boards/cardputer.h"
namespace sapper {
inline constexpr const BoardProfile& kActiveBoard = kCardputerProfile;
}
#define SAPPER_BOARD_HAS_DISPLAY 1
#elif defined(SAPPER_BOARD_M5STICKC_PLUS2)
#include "config/boards/m5stickc_plus2.h"
namespace sapper {
inline constexpr const BoardProfile& kActiveBoard = kM5StickCPlus2Profile;
}
#define SAPPER_BOARD_HAS_DISPLAY 1
#elif defined(SAPPER_BOARD_T_EMBED)
#include "config/boards/t_embed.h"
namespace sapper {
inline constexpr const BoardProfile& kActiveBoard = kTEmbedProfile;
}
#define SAPPER_BOARD_HAS_DISPLAY 1
#elif defined(SAPPER_BOARD_ESP32_3248S035C)
#include "config/boards/esp32_3248s035c.h"
namespace sapper {
inline constexpr const BoardProfile& kActiveBoard = kEsp32_3248S035CProfile;
}
// SAPPER_BOARD_HAS_DISPLAY is deliberately NOT defined: the CYD ships headless this slice (hasDisplay=false),
// so no LGFX_* class is compiled and the HAL resolves to NullDisplay (ADR-0002 #4). The 320x480 ST7796 panel
// is a later slice needing a canvas-free renderer (ADR-0082 context). An undefined macro evaluates to 0 in the
// `#if SAPPER_BOARD_HAS_DISPLAY` guards, which is the screenless branch — no #ifdef juggling needed.
#else
#error "No SAPPER_BOARD_* selected — define one in platformio.ini build_flags"
#endif
