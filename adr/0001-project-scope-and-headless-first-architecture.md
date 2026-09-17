---
id: '0001'
title: "Project scope and headless-first architecture"
type: architecture
status: accepted
date: 2026-09-17
supersedes: []
superseded_by: []
---

## Context

The WiFi Sapper is a spin-off of The Adversary (`../adversary`), reusing its proven
subsystems but for a different shape of product. The Adversary is an **interactive
multi-tool**: a human drives it through a Cardputer keyboard or M5Stick buttons, picking
targets and attacks screen by screen. Its handshake auto-hunt lives *inside* the UI —
`HandshakeScreen` owns the `AutoHunt` state machine (`selectNextTarget`, `updateAutoHunt`),
and the network path assumes a keyboard exists to type the WPA-SEC API key and WiFi
credentials (`SettingsManager::getWpaSecKey`, entered via `character_selector`).

The Sapper is the opposite: an **autonomous appliance**. It does one thing forever with no
operator present — auto-hunt handshakes endlessly, auto-upload each new capture to
`wpa-sec.stanev.org`, and once an hour fetch cracked results back and announce any newly
recovered passwords. It must run on a wide range of ESP-32 hardware, much of which has **no
keyboard, no screen, and no SD card** (bare dev-kits, some CYD variants, M5StickC). Two of
the Adversary's assumptions therefore break at the root:

- **UI-owned control flow.** An engine welded to a screen cannot run on a screenless board,
  and cannot be unit-tested natively (the Adversary excludes all of `src/ui/` from its
  `native` test env for exactly this reason).
- **Keyboard-entered secrets.** There is no way to type a 32-character API key on a board
  with no input hardware.

A decision is needed on the product boundary (what the Sapper is and is not) and on the
top-level software architecture, before any subsystem is ported.

## Decision

**Scope.** The Sapper's job is exactly one autonomous loop:
*hunt → capture → upload → (hourly) sync cracked results → alert*. It ports from The
Adversary only the subsystems that loop needs: handshake capture, deauth, station scan,
pcap write, the WPA-SEC upload/download flow with its TLS/heap plumbing, NTP time sync, and
the memory-stabilisation machinery. The Adversary's other tools — RF/sub-GHz, IR, BLE,
RFID, BadUSB, wardriving, evil-twin/karma portals, the packet sniffer/proxy — are **out of
scope**. OTA update is deferred (see `LEDGER.md`).

**Headless-first, engine as the core.** The capture→upload→sync logic is a standalone
engine with **zero dependency on any display, input device, or storage medium**. The
Adversary's `AutoHunt` state machine is *extracted* out of `HandshakeScreen` into a headless
`HuntEngine`, not ported in place. The engine compiles and is unit-tested on the `native`
target with no hardware present.

**Surfaces are observers, never owners.** Every user-facing surface — an optional display,
an onboard/RGB LED, a served web dashboard, an outbound push webhook — is a *subscriber* to
the ported `core/event_bus`. The engine publishes facts ("capture saved", "upload ok",
"password cracked"); surfaces react. A screenless board simply registers no display
subscriber; the engine is unchanged. This boundary is the load-bearing decision of this
ADR: **nothing in the engine may call a surface directly.**

**One retained view.** When a display is present it renders a *lightweight scanner view* —
nearby networks, the current target, and live capture status — as a read-only event-bus
observer. This is the sole UI view carried over from The Adversary. It is deliberately
read-only: it does **not** let an operator pick targets, because interactive target
selection would re-weld control flow into the screen and break the headless-first boundary
above. On a screenless board the same information is on the web dashboard.

**Mechanism decisions this ADR spawns** (each recorded in its own ADR as it is picked up, so
each stays a single historical claim rather than being duplicated here):

- Display + board portability — a Board Profile capability model over a board-agnostic
  graphics abstraction (LovyanGFX auto-detect). → ADR-0002.
- Configuration and secrets without a keyboard — a first-boot captive AP portal writing WiFi
  credentials and the WPA-SEC API key to NVS. → its own ADR with slice-2.
- Memory strategy — PSRAM-aware canvas/palette policy with the Adversary's canvas-purge
  "nuclear option" demoted to a no-PSRAM fallback. → its own ADR with the engine's upload
  slice.

**Authorisation posture.** An unattended, endlessly-deauthing appliance is a heavier
legal/ethical posture than an operator-driven tool. The ported SSID whitelist is retained,
and an **allowlist-only mode** (operate solely on explicitly-authorised BSSIDs) is a
first-class, portal-configurable option (tracked in `LEDGER.md`).

## Consequences

- The engine is testable on `native` with no hardware, and portable to any board regardless
  of its peripherals — the entire justification for this project's "runs anywhere" goal.
- Adding a new surface (e.g. the push webhook) is additive: subscribe to the event bus, touch
  no engine code. Adding a new board is a Board Profile plus a LovyanGFX config (ADR-0002),
  not edits scattered across the codebase.
- Any future contribution that reaches from the engine into a surface, or re-welds control
  flow into a screen, violates this ADR and must be rejected at review or justified by a
  superseding ADR — never a silent exception.
- The reused deauth path inherits the Adversary's hard toolchain constraint: raw deauth TX
  requires the `pioarduino` platform plus Bruce's patched `esp32-arduino-libs` and the
  `weaken_deauth_pre.py` pre-script. The Sapper's `platformio.ini` must replicate these
  pins exactly (this lands in slice-1's board bring-up), or deauth silently fails to
  transmit.
- Excluding the Adversary's other tools keeps the firmware within the ~205 KB internal-SRAM
  budget of the no-PSRAM reference board (M5Stack Cardputer) with headroom for the TLS
  upload path.
