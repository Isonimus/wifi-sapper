---
id: '0058'
title: "Maintenance entry: runtime 'm' keypress on the TCA8418 keyboard reboots into Maintenance"
type: slice
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Goal

Implement ADR-0057: make Maintenance reachable on the Cardputer ADV by pressing the `m` key while
hunting, which persists a fail-safe reboot-intent and reboots into a clean Maintenance boot. Remove
the dead GPIO0 windowed-poll entry path that the field proved unreachable on this board.

- **Pure reboot-intent (`config/maintenance_intent`):** `shouldEnterMaintenanceOnBoot(resetReason,
  storedWord)` returns true only for `(ESP_RST_SW && storedWord == kMaintenanceIntentMagic)`; a helper
  models the one-shot read (returns the decision and yields the cleared word). Host-tested.
- **Pure key predicate (`hal/input/tca8418_event`):** `decodeKeyEvent(byte)` → `{pressed,row,col,code}`
  and `isMaintenanceKey(byte)` (true only for a *press* of keycode `kMaintenanceKeycode = 48`, the `m`
  key). Host-tested; no I²C.
- **Device driver (`hal/input/tca8418_keyboard`):** minimal TCA8418 bring-up (I²C `0x34` on 8/9, INT
  `11`) and `maintenanceKeyPressed()` using the ADR-0057 register sequence + a GPIO11 ISR flag.
  Device-only (`#ifndef UNIT_TEST`).
- **Boot gate (`main.cpp`):** `maintenanceRequested()` reads the RTC intent (`RTC_NOINIT_ATTR`) via the
  pure decision + one-shot clear, instead of polling `bootButtonPin`; the ADV profile sets
  `bootButtonPin = -1`. The `Ready` branch of `loop()` polls the keyboard and, on the `m` key, sets the
  intent and `ESP.restart()`s.
- **Docs/§4:** update `README.md` (entry is a runtime `m` press, not a boot button), `CLAUDE.md` §4 #21
  (cite ADR-0057) and #3 (drop the ADR-0053 window carve-out), and add a keyboard-keycode probe.

## Definition of Done

**Scenario A — the reboot-intent is honoured only on a software reset with the sentinel, and is
one-shot (pure).**
- **Given** the RTC word holds `kMaintenanceIntentMagic`
- **When** `shouldEnterMaintenanceOnBoot(reset, word)` is evaluated for each reset reason
- **Then** it is `true` only for `ESP_RST_SW`, and `false` for `ESP_RST_POWERON` / `ESP_RST_BROWNOUT`
  / any non-sentinel word; and the one-shot read yields a cleared word so a re-evaluation is `false`.
- **Proof:** `test/test_maintenance_intent` — fails before (no such function) and passes after. See
  `## As built`.

**Scenario B — the `m` key is recognised, nothing else is (pure).**
- **Given** a raw TCA8418 event byte
- **When** `isMaintenanceKey(byte)` runs
- **Then** it is `true` for a *press* of keycode 48 (byte `0xB0` = bit7-set | 48), and `false` for its
  release (byte `0x30`), for other keycodes (press or release), and for the empty event `0x00`; and
  `decodeKeyEvent` splits state/keycode/row/col per the ADR-0057 decode.
- **Proof:** `test/test_maintenance_intent` (key-predicate cases) — fails before / passes after. See
  `## As built`.

**Scenario C — a running device enters Maintenance from an `m` press, and a power-cycle does not
(review + on-device).**
- **Given** a provisioned ADV hunting in `Ready`
- **When** the operator presses `m` on the keyboard
- **Then** the device logs the intent, reboots, and comes up in Maintenance (the `Sapper-Maint-XXXX`
  SoftAP is raised); **and** a plain power-cycle (no keypress) boots back into hunting, never
  Maintenance.
- **Proof:** the TCA8418 keycode for `m` is confirmed `48` by the keyboard probe (`verify:keyboard`,
  a one-shot probe whose number lands in ADR-0057); the runtime-press → reboot → Maintenance path and
  the ISR/`loop()` wiring are review-only (a physical keypress cannot be driven headlessly, as the
  physical press was in ADR-0053). See `## As built`.

## Verification

- **Reboot-intent + key predicate (Scenarios A, B)** — `pio test -e native`
  (`test_maintenance_intent`), fail-before/pass-after recorded in `## As built`.
- **`m` keycode (Scenario C, one-shot)** — `scripts/0058-keyboard-probe.mjs` (`verify:keyboard`): a
  `SAPPER_TEST_KB` build logs each decoded key event; the operator presses `m` and the probe confirms
  `code=48`. A probe (named `*-probe.mjs`, not `*-verify.mjs`), so exempt from the R11 wired-verify
  rule — its number lands in the ADR.
- **Runtime entry + power-cycle-safety (Scenario C)** — review-only: `loop()` poll → intent →
  `ESP.restart()`, and the `esp_reset_reason()` gate that keeps a cold boot out of Maintenance.

## As built

Built as designed, with the pure/impure split ADR-0057 called for.

- **Pure units, host-tested (Scenarios A, B).** `config/maintenance_intent.{h,cpp}` —
  `shouldEnterMaintenanceOnBoot(esp_reset_reason_t, uint32_t)` and `consumeMaintenanceIntent()` (the
  one-shot read, returning the decision + the cleared word). `hal/input/tca8418_event.h` (header-only)
  — `decodeKeyEvent(byte)` → `{pressed, keycode, row, col}` and `isMaintenanceKey(byte)` against
  `kMaintenanceKeycode = 48`. The native lane resolves `<esp_system.h>` to a new `native/mock/esp_system.h`
  (the enum only), via the existing `-I native/mock` seam (ADR-0008). `test/test_maintenance_intent`
  covers both scenarios — **7 tests, green (the fail-before for a brand-new unit is the unresolved
  symbols; the suite does not link without these files). Each was confirmed to bite by manually mutating
  the two predicates — dropping the `ESP_RST_SW` gate and the press check each turned the relevant tests
  red — then reverting.**
- **Device glue (Scenario C, review-only).** `hal/input/tca8418_keyboard.{h,cpp}` (`#ifndef UNIT_TEST`):
  I²C `0x34` on SDA 8 / SCL 9, a GPIO11 falling-edge ISR flag, the Adafruit-derived init sequence, and a
  single FIFO-drain primitive (`drainRawEvents`) that both `maintenanceKeyPressed()` and the probe reuse
  through the pure decode. `main.cpp`: an `RTC_NOINIT_ATTR` intent word; `maintenanceRequested()` now
  reads+consumes it via `consumeMaintenanceIntent(esp_reset_reason(), …)` instead of polling a pin; the
  `Ready` branch of `loop()` polls the keyboard and, on `m`, sets the sentinel and `ESP.restart()`s; the
  keyboard is brought up on entering Ready, gated on `InputKind::Keyboard`, failing loud-not-fatal.
  `config/boards/cardputer.h` drops `bootButtonPin` to `-1`; the dead GPIO0-window wiring and the
  `entry_gesture.h` include are removed from `main.cpp` (the header stays in-tree for a future button board).
- **Keycode confirmation (Scenario C, on-device — pending operator hardware run).** `keyboard_probe.{h,cpp}`
  + `scripts/0058-keyboard-probe.mjs` (`verify:keyboard`, `SAPPER_TEST_KB`). `kMaintenanceKeycode = 48` is
  ADR-0057's best derivation from M5's ADV key table; the probe logs each keypress's decoded key number so
  the operator confirms `m` = 48 on real hardware. **If the probe reports a different number, amend
  ADR-0057 and `kMaintenanceKeycode` together** (the ADR records the measurement, not the guess). The
  physical `m`-press → reboot → Maintenance path and the power-cycle-safety are review-only (a keypress and
  a reset reason cannot be driven headlessly) — reviewed against the code, not yet exercised on-device.
- **Builds.** `pio test -e native` green (full suite); `pio run -e cardputer` (shipped) and
  `SAPPER_TEST_KB=1 pio run -e cardputer_testhooks` (probe) both compile clean.
- **Docs/§4.** README's Maintenance section rewritten to the runtime `m` press + a `verify:keyboard` table
  row; §4 #21 cites ADR-0057 as the entry mechanism (superseding ADR-0053 in full) with `test_maintenance_intent`
  as its proof; §4 #3's ADR-0053 window carve-out removed; the slice-8 LEDGER item now defers per-board
  entry generalisation + full keyboard bring-up to ADR-0057.

Deviation from the slice plan: none of substance. The driver exposes one extra method (`drainRawEvents`)
beyond a bare `maintenanceKeyPressed()` so the entry predicate and the keycode probe share a single
I²C-touching FIFO drain rather than duplicating register access — in service of the same entry, not scope creep.

**Adversarial pass (Sonnet 5, blind to intent / aware of law).** Two real gaps found and fixed:
- **Swallowed I²C errors / possible permanent wedge (fixed).** `Tca8418Keyboard::begin()` now checks
  every init write's ACK and returns false on a NACK (loud via the caller's `[KB]` log), and
  `drainRawEvents()` gates on the INT *level* (a local GPIO read, not I²C) in addition to the ISR edge
  flag — so a transient failure on the INT-ack write, or a missed edge, self-heals on the next poll
  instead of silently wedging runtime entry for the rest of the boot (§3 fail-loud).
- **Intent not consumed on the first provisioned boot (fixed).** `main.cpp` now reads+consumes the RTC
  intent *unconditionally* every boot and gates only the resulting decision on `hasCreds`; previously the
  `hasCreds &&` short-circuit meant the very first provisioned boot (itself an `ESP_RST_SW` from the
  portal-save reboot) could read never-cleared cold-boot RTC garbage and, if it matched the sentinel,
  enter Maintenance. Now the word is always cleared, matching the pure module's "reading consumes" contract.
- Dead code (raised by the reviewer, decided by the operator): `config/entry_gesture.h` +
  `test/test_entry_gesture` were unreachable once the GPIO0 poll was retired. **Deleted** (§3: dead code
  is deleted, not parked); the `bootButtonPin` field stays as a board-fact declaration for a future
  pollable-button board. ADR-0057 records the delete-not-park rationale.

**On-device follow-up (boyscout fix of a latent ADR-0039 defect this slice exposed).** Once entry worked
on hardware, pressing `m` rebooted twice into hunting and `Sapper-Maint-XXXX` never appeared. The on-device
backtrace showed a **stack-canary panic in `CrackedManifest`'s constructor** at `runMaintenance`
(`main.cpp`): the manifest is a ~28 KB object (`CrackedEntry entries_[256]`) allocated as a **stack local**
on the ~8 KB loopTask stack — it overflowed the instant it was constructed, before `WiFi.softAP()` (hence
no AP), and the panic was reboot #2 (the one-shot intent, already consumed, sent the next boot to hunting).
Fixed by moving the manifest + its store to **static storage** (`.bss`), mirroring the hunt path's
`g_manifest` global. Invisible until now because entry was unreachable and a stack overflow is not
host-testable; recorded as a LEDGER audit item (verify:maintenance never reached the served dashboard on
hardware — needs a real on-device run now that entry works). Shipped build recompiles clean.
