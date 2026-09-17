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
#else
#error "No SAPPER_BOARD_* selected — define one in platformio.ini build_flags"
#endif
