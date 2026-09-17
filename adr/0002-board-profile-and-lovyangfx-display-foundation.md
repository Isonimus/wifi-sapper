---
id: '0002'
title: "Board Profile capability model over explicit per-board LovyanGFX configs"
type: architecture
status: accepted
date: 2026-09-17
supersedes: []
superseded_by: []
---

## Context

ADR-0001 commits the Sapper to running the *same* firmware across hardware with very
different peripherals — M5Stack (Cardputer, StickC), CYD variants, Lilygo boards, and bare
ESP-32 dev-kits with no screen, keyboard, or SD at all. Two concrete obstacles inherited
from The Adversary must be removed for that to be possible:

- **Hardware differences are encoded as scattered `#ifdef TARGET_*`.** In the Adversary,
  `TARGET_CARDPUTER` / `TARGET_M5STICK` are tested in `config/config.h`, `config/pins.h`,
  `hal/display/canvas_types.h`, and across many `ui/screens/*`. Adding a board means finding
  and editing every one of those sites, and a missed site fails silently at runtime on the
  new board only.
- **The display layer is bound to M5 hardware.** `canvas_types.h` hard-wires
  `using Canvas = M5Canvas` (M5GFX, a LovyanGFX derivative that ships M5-specific board
  bring-up). A CYD clone or a bare ST7789/ILI9341 panel cannot be driven through it at all.

Display specifically needs a board-agnostic graphics layer. Of the three mainstream ESP-32
graphics libraries, all use *explicit per-board display configuration* as the robust path:
TFT_eSPI selects a per-board `User_Setup.h`; Arduino_GFX constructs explicit databus + panel
objects; and LovyanGFX's own documented way to drive an arbitrary panel is a class deriving
from `lgfx::LGFX_Device`. LovyanGFX's `LGFX_AUTODETECT` recognises only a fixed device list
(the M5 family and a handful of others), so it cannot cover CYD clones or bare panels —
multi-board ESP-32 firmwares (Bruce, ESP32Marauder) therefore all ship explicit configs
selected by a build flag. Relying solely on autodetect is the fragile choice; explicit
configs keyed off a board descriptor is the conventional, robust one.

## Decision

**1. One Board Profile per board, queried at compile time.** A single `BoardProfile`
descriptor replaces the scattered `TARGET_*` tests. It declares what the board *has*, not
what it *is*: display presence + geometry (width, height, rotation, canvas colour depth),
input kind (`none` / `keyboard` / `buttons` / `touch`), storage (`sd` / `littlefs` / `none`),
`hasPsram`, status-LED kind + pin(s), battery-ADC pin, and the GPIO/bus pins the engine and
peripherals need. Core code branches on *capabilities* (`profile.hasDisplay`,
`profile.storage`), never on a board identity. Exactly one board is selected per build via a
single `SAPPER_BOARD_*` flag. The profile is a `constexpr`/compile-time constant — no RTTI,
no heap — to stay within the no-PSRAM SRAM budget (ADR-0001).

**2. Explicit per-board LovyanGFX config classes.** Each board that has a display ships a
small `LGFX_<board>` class deriving from `lgfx::LGFX_Device`, configuring panel, bus, and
backlight explicitly. `LGFX_AUTODETECT` is permitted only as a convenience for boards
LovyanGFX genuinely recognises; it is never the mechanism a new board relies on. The active
board's config class is selected by the same `SAPPER_BOARD_*` flag as its profile.

**3. The canvas type is board-agnostic.** The Adversary's M5GFX-bound `canvas_types.h` is
replaced by `LGFX_Sprite` over LovyanGFX. Canvas colour depth is a Board Profile field, not a
constant — the memory ADR (forthcoming, with the upload slice) tunes it per board (reduced
palette on no-PSRAM boards).

**4. A screenless board sets `hasDisplay = false`.** The display HAL then resolves to a null
implementation and no `LGFX_*` class is compiled in. This is ADR-0001's optional-surface
model made concrete: the engine is unchanged, the display simply is not a subscriber.

**5. The Cardputer is brought up through LovyanGFX directly, not M5GFX/M5Cardputer.** The
reference board gets the first `LGFX_Cardputer` config + `BoardProfile`, deliberately *not*
reusing the Adversary's M5 graphics stack — so the portable path is exercised on day one
rather than discovered to be a dead end at the last board. The exact panel parameters
(driver, offsets, rotation) are confirmed empirically in slice-1's bring-up and its verify
artifact, not asserted here.

## Consequences

- Adding a board is bounded and mechanical: one `BoardProfile`, one `LGFX_*` config (if it
  has a screen), one `SAPPER_BOARD_*` flag — no edits scattered across the tree, and a
  missing capability is a compile-time absence rather than a silent runtime failure.
- We take on writing and maintaining an `LGFX_*` config per board — the cost of not using
  M5GFX's built-in M5 bring-up. ADR-0001's portability mandate is what pays for it; the same
  cost buys CYD and bare-panel support that M5GFX cannot give at any price.
- Core code that branches on a board *identity* instead of a capability violates this ADR and
  is rejected at review — that is the scattered-`#ifdef` failure this ADR exists to remove.
- Board Profile stays compile-time; a future need for runtime board detection would be a new
  decision (superseding ADR), not an in-place change, because heap-allocating the profile
  would breach the SRAM budget this relies on.
