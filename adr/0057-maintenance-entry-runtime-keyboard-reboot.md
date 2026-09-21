---
id: '0057'
title: "Maintenance entry via a runtime keyboard press + reboot-into-Maintenance (Cardputer ADV TCA8418)"
type: architecture
status: accepted
date: 2026-09-21
supersedes: ['0053']
superseded_by: []
---

## Context

ADR-0053 made Maintenance entry a **debounced GPIO0 press during a bounded post-boot window**,
on the premise that "nearly every ESP32 board wires GPIO0 (BOOT) to a pressable button"
(`board_profile.h`, `bootButtonPin = 0` for the Cardputer). That premise is **false for the
reference board**, and the feature was unreachable in the field because of it.

Measured, not assumed. A field diagnostic (an uncommitted build that widened the window to 8 s and
logged the raw GPIO0 level every 250 ms and on every LOW read) was run on the operator's
**Cardputer ADV**. Across a full 8-second window with the operator holding the side button the whole
time, GPIO0 read `1` (HIGH) on **every** sample — not one LOW. The window logic itself was correct
(`hasCreds=1`, the poll ran the full duration and would have early-exited on a LOW); the signal
simply never arrives on GPIO0.

The cause is hardware, confirmed against M5Stack's ADV documentation:

- The **Cardputer ADV** replaces the original Cardputer's GPIO keyboard matrix with a **TCA8418 I²C
  keypad-scan controller** — I²C address `0x34` on `SDA=GPIO8 / SCL=GPIO9`, interrupt on `GPIO11`
  (active-low). Every key — and there is no separate general-purpose front button — is read over I²C
  from the controller's event FIFO, not from an MCU GPIO.
- **GPIO0 on the ADV is a boot strap only**: the documentation describes it exclusively as *"hold G0
  while applying power to enter download mode."* Holding it at reset-release drops the ROM into serial
  download mode (the very trap ADR-0053 was written to escape); there is no runtime-pressable button
  wired to GPIO0 that firmware can poll. The M5Unified board table lists `BtnA = GPIO0` for the ADV,
  but that reflects the strap line, not a pressable runtime button — which is exactly what the
  measurement showed.

So ADR-0053's engineering was sound but built on a wrong hardware model: it moved the *timing* of the
read (post-boot, not hold-at-power-on) but kept the *source* (a pressable GPIO0 button) that the
reference board does not have. The reachable input on the ADV is the keyboard, behind the TCA8418.

A second force: the operator asked whether modes should be switchable **while running** instead of
only at boot. A *live* radio reconfiguration is refused (§4 #21 / ADR-0039: Maintenance is a
hunt-suspended phase entered only from a clean boot — the hunt engine owns the radio exclusively
under §4 #10/#11/#16, and tearing that down mid-flight to raise a SoftAP reopens the callback-context
concurrency hazards those invariants exist to prevent). But the operator's underlying need — "let me
enter Maintenance without a power-on timing window I cannot hit" — is legitimate and is met by a
runtime *trigger* that reboots into a clean Maintenance boot, the inbound mirror of ADR-0043's
reboot-scoped resume/re-provision controls.

## Decision

1. **Retire the GPIO0 windowed poll as the entry mechanism.** There is no post-boot press window.
   `maintenanceRequested()` no longer reads `bootButtonPin`; the ADV's `bootButtonPin` is dropped to
   `-1` (no pollable button), and the `PressDebouncer`/window unit (`entry_gesture.h`) — dead once the
   poll is retired — is deleted with its test. This removes the timing-window UX problem entirely.

2. **Enter Maintenance by a runtime keyboard press that reboots into it.** While hunting (the `Ready`
   phase), a press of the designated **Maintenance key** on the TCA8418 keyboard sets a persisted
   *reboot-intent* and issues `ESP.restart()`. The subsequent clean boot reads the intent and routes
   to Maintenance through the existing pure `decideBootPhase()` gate — **Maintenance still enters only
   from a clean boot**, so §4 #21 holds unchanged. The operator may press at any time; there is no
   window to catch.

3. **The reboot-intent is RTC-scoped, not flash-scoped**, and fail-safe against cold-boot garbage. A
   `RTC_NOINIT_ATTR` word survives the software `ESP.restart()` we issue but is undefined after a
   power-on/brownout. The intent is honoured on the next boot **only if both** hold:
   `esp_reset_reason() == ESP_RST_SW` **and** the word equals a fixed magic sentinel
   (`kMaintenanceIntentMagic`). A cold power-on (reset reason `POWERON`/`BROWNOUT`, or a
   non-sentinel word) is ignored — so a power-cycled or lost unit boots to hunting, never silently
   into Maintenance. The intent is **one-shot**: it is cleared (word zeroed) the moment it is read,
   before the phase is decided, so a crash inside Maintenance cannot loop the device back into it.
   This is not NVS — it wears no flash and does not touch the ADR-0006 #6 provisioning seam.

4. **A minimal, purpose-built TCA8418 driver** (device-only HAL, `hal/input/`), not the M5Cardputer
   library. This repo depends only on LovyanGFX; pulling in M5Cardputer would drag M5GFX and conflict
   with the display stack. The driver does only what entry needs — bring the controller out of its
   power-on sleep and detect the Maintenance key — using the register sequence measured from M5's own
   `Adafruit_TCA8418`:
   - init: `GPIO_DIR_{1,2,3}=0x00`, `GPI_EM_{1,2,3}=0xFF`, `GPIO_INT_LVL_{1,2,3}=0x00`,
     `GPIO_INT_EN_{1,2,3}=0xFF`; `matrix(7,8)` → `KP_GPIO_1=0x7F`, `KP_GPIO_2=0xFF`; flush the event
     FIFO; `CFG |= (GPI_IEN|KE_IEN)` (`0x03`).
   - event decode (pure, host-tested): a FIFO byte's bit7 is the press/release state and
     `(byte & 0x7F) - 1` is the electrical keycode, `row = code/10`, `col = code%10`.
   - a `GPIO11` falling-edge ISR sets a volatile flag; the driver drains the FIFO when that flag is set
     **or** the INT line reads still-asserted (active-low). The level check is a local GPIO read, not
     I²C — so a quiet hunt path still touches the bus only when there is an event — but it makes entry
     self-healing: a transient I²C error on the INT-ack write, or a missed edge, leaves INT low and the
     next poll re-drains rather than wedging entry until the next power-cycle (a pure edge trigger never
     re-fires on an already-low line). Every init write's ACK is checked and a NACK fails `begin()` loud
     (§3 fail-loud, no swallowed errors) — both hardenings added after the slice-0058 adversarial pass.

5. **The Maintenance key is the `m` key** ("m" for Maintenance). Its raw TCA8418 keycode is derived
   from the ADV keymap as `48` and is **confirmed on-device by the verify script** before the slice is
   accepted (a keycode is a measurable fact; the ADR records the measurement, not the guess). The
   match is a pure predicate (`isMaintenanceKey(rawEventByte)`), host-tested against the documented
   keycode; the physical→keycode correspondence is what the on-device verify pins.

6. **Pure/impure split, per the standing seams.** The reboot-intent decision
   (`shouldEnterMaintenanceOnBoot(resetReason, storedWord) -> bool` and the one-shot clear) and the
   key predicate are pure and compiled on the native lane; the TCA8418 I²C driver, the GPIO11 ISR, the
   `loop()` poll, and the `ESP.restart()` are device-only glue verified on hardware. The device seam
   feeds the same pure `decideBootPhase()` input `maintenanceRequested` that ADR-0053 fed — the gate
   is unchanged, only its source moved from a GPIO to the RTC intent.

7. **Scope: the reference board (Cardputer ADV).** Boards with a genuinely pollable GPIO0 button
   (e.g. an M5Stick) can keep a press-based entry when they are brought up; generalising the
   per-board entry-input capability (button vs keyboard vs chord) is left to the slice-8 hardware
   broadening and is not decided here. This ADR does not add a second mechanism to the shipped ADV
   build — it replaces the one that never worked.

## Consequences

- Maintenance becomes reachable on the reference hardware, by an input the board actually has, with
  no power-on timing window — directly resolving the field defect and the operator's "switch without a
  window" need without a live radio swap.
- The keyboard is now **partially** brought up: only the TCA8418 init and single-key detection that
  entry requires. Full keyboard text entry (portal typing, dashboard input) remains deferred to
  slice-8; this ADR does not claim it.
- A new runtime input path exists on the hunt loop, but it is interrupt-gated and touches only I²C
  (never the Wi-Fi radio or the promiscuous RX callback of §4 #10), so it does not weaken the radio
  ownership invariants. It is not a §4 #3 blocking state (it is an edge-triggered check in the
  non-blocking `loop()`, not an interactive dwell), so it pumps no serial — and, unlike ADR-0053's
  window, there is no boot dwell to carve out at all; the §4 #3 carve-out ADR-0053 added is removed.
- `RTC_NOINIT` intent is deliberately volatile across power loss: the failure mode is "a cold boot
  never enters Maintenance," which is the safe direction (a walked-away/lost unit resumes hunting).
- ADR-0053 is superseded in full (its entire subject — the entry gesture — is replaced). ADR-0039's
  other Maintenance decisions (dashboard, PSK exposure, SoftAP hardening, panel, controls) and
  ADR-0043's write-controls are untouched. §4 #21 is updated to cite ADR-0057 as the entry mechanism
  and #3's ADR-0053 window carve-out is dropped, both in the same commit as this ADR.
- The `bootButtonPin` **field** stays in the `BoardProfile` fact table (ADR-0002) — a cheap board-fact
  declaration like the other pin fields, `-1` on the ADV, available to a future board that genuinely
  wires a pollable non-strapping button. The `entry_gesture.h` window/debounce **unit** and its test,
  by contrast, are executable logic with no remaining caller, so they are **deleted, not parked** —
  version control remembers them, and a future button-board rebuilds debounce against its own real
  requirements rather than inheriting a speculatively-retained one (§3: dead code is deleted; slice-0054's
  frozen account stays historically true for its date). The dead wiring is removed from `main.cpp` too.
