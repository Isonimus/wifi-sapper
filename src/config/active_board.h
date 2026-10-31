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
#define SAPPER_BOARD_HAS_DISPLAY 1
// The ST7796 320x480 panel is lit via the direct-to-panel renderer (ADR-0084, slice-0085): the env also
// sets SAPPER_DISPLAY_DIRECT_PANEL, which picks DirectPanelDisplay over the sprite-backed LgfxDisplay (a
// full-frame 320x480 sprite is 307200 B, infeasible on this no-PSRAM board's ~110 KB largest free block,
// ADR-0082). slice-0083 shipped this board headless; the display is the additive slice-0085.
#elif defined(SAPPER_BOARD_ATOM_ECHO)
#include "config/boards/atom_echo.h"
namespace sapper {
inline constexpr const BoardProfile& kActiveBoard = kAtomEchoProfile;
}
// No SAPPER_BOARD_HAS_DISPLAY: the Atom Echo is headless (hasDisplay=false) with no panel at all, so no
// LGFX_* class compiles (ADR-0002 #4). The first board to ship with the guard *permanently* absent: the
// CYD exercised this path transiently at slice-0083, then slice-0085 gave it a panel. slice-0105.
#else
#error "No SAPPER_BOARD_* selected — define one in platformio.ini build_flags"
#endif
