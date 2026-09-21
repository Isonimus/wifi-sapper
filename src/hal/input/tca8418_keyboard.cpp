/**
 * @file tca8418_keyboard.cpp
 * @brief TCA8418 bring-up + Maintenance-key detection for the Cardputer ADV (ADR-0057). Device-only.
 */
#ifndef UNIT_TEST

#include "hal/input/tca8418_keyboard.h"

#include <Arduino.h>
#include <Wire.h>

#include "hal/input/tca8418_event.h"

namespace sapper {
namespace {

// Cardputer ADV wiring for the TCA8418 (M5Stack ADV documentation). Hardcoded to the reference board
// on purpose: ADR-0057 decision 7 scopes this driver to the ADV and defers per-board generalisation
// to slice-8. Named, not bare, so the wiring is traceable rather than a magic pin in a Wire call.
constexpr uint8_t kTca8418Address = 0x34;
constexpr uint8_t kSdaPin = 8;
constexpr uint8_t kSclPin = 9;
constexpr uint8_t kIntPin = 11;  // active-low, open-drain: needs a pull-up and a falling-edge ISR.

// TCA8418 register addresses (datasheet; the subset the init sequence and FIFO drain touch).
constexpr uint8_t kRegCfg = 0x01;         // configuration: interrupt-enable bits live here.
constexpr uint8_t kRegIntStat = 0x02;     // interrupt status; write 1s to the serviced bits to clear.
constexpr uint8_t kRegKeyLckEc = 0x03;    // low nibble = queued key-event count.
constexpr uint8_t kRegKeyEventA = 0x04;   // pop one event byte off the FIFO (0x00 when empty).
constexpr uint8_t kRegGpioIntEn1 = 0x1A;  // +1,+2 for banks 2,3.
constexpr uint8_t kRegKpGpio1 = 0x1D;     // +1,+2: which GPIOs form the key-scan matrix.
constexpr uint8_t kRegGpiEm1 = 0x20;      // +1,+2: route GPI changes into the event FIFO.
constexpr uint8_t kRegGpioDir1 = 0x23;    // +1,+2: GPIO direction (0 = input).
constexpr uint8_t kRegGpioIntLvl1 = 0x26; // +1,+2: interrupt level.

// CFG bits: enable a key-event interrupt (KE_IEN) and a GPI interrupt (GPI_IEN) on INT.
constexpr uint8_t kCfgKeIen = 0x01;
constexpr uint8_t kCfgGpiIen = 0x02;

// INT_STAT bits serviced on each drain: key-event (K_INT) and GPI (GPI_INT), cleared write-1-to-clear.
constexpr uint8_t kIntStatServiced = 0x03;

// The ADV matrix is 7 rows x 8 columns: rows on KP_GPIO bank 1 (bits 0..6 = 0x7F), columns on bank 2
// (bits 0..7 = 0xFF), bank 3 unused. Matches Adafruit_TCA8418::matrix(7, 8).
constexpr uint8_t kMatrixRowsMask = 0x7F;
constexpr uint8_t kMatrixColsMask = 0xFF;

// The FIFO is 10 deep; bound the drain a little above that so a wedged bus can never spin forever.
constexpr uint8_t kFifoDrainLimit = 16;

// Set by the GPIO11 ISR, read/cleared on the app task. `volatile` because the ISR and loop() race on it.
volatile bool g_eventPending = false;

void IRAM_ATTR onKeyInterrupt() { g_eventPending = true; }

bool writeRegister(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(kTca8418Address);
    Wire.write(reg);
    Wire.write(value);
    return Wire.endTransmission() == 0;
}

uint8_t readRegister(uint8_t reg) {
    Wire.beginTransmission(kTca8418Address);
    Wire.write(reg);
    Wire.endTransmission();
    Wire.requestFrom(kTca8418Address, static_cast<uint8_t>(1));
    return Wire.available() ? static_cast<uint8_t>(Wire.read()) : 0;
}

}  // namespace

bool Tca8418Keyboard::begin() {
    Wire.begin(kSdaPin, kSclPin);

    // Probe first: if the controller does not ACK its address, the board is not an ADV (or the bus is
    // dead). Report the failure loud rather than issuing a sequence of writes into the void (§3).
    Wire.beginTransmission(kTca8418Address);
    if (Wire.endTransmission() != 0) return false;

    // Adafruit_TCA8418 begin() equivalent: all GPIOs inputs, route their edges into the FIFO, level-low
    // interrupts, all interrupts enabled — across the three banks. Every write's ACK is checked: after
    // the address probe ACKed, a NACK here means the controller is present but not accepting config, so
    // the keyboard would be silently half-initialised (no FIFO routing / no INT). Fail loud — return
    // false so the caller logs it and runtime Maintenance entry is known-unavailable, not silently dead
    // (§3 no swallowed errors). `ok` accumulates so one bad write fails begin() without masking (§3).
    bool ok = true;
    for (uint8_t bank = 0; bank < 3; ++bank) {
        ok &= writeRegister(kRegGpioDir1 + bank, 0x00);
        ok &= writeRegister(kRegGpiEm1 + bank, 0xFF);
        ok &= writeRegister(kRegGpioIntLvl1 + bank, 0x00);
        ok &= writeRegister(kRegGpioIntEn1 + bank, 0xFF);
    }
    // matrix(7, 8): claim rows on bank 1 and columns on bank 2 as the key-scan matrix.
    ok &= writeRegister(kRegKpGpio1 + 0, kMatrixRowsMask);
    ok &= writeRegister(kRegKpGpio1 + 1, kMatrixColsMask);

    // Flush any power-on events, then clear the interrupt status so INT starts de-asserted.
    for (uint8_t i = 0; i < kFifoDrainLimit && readRegister(kRegKeyEventA) != 0; ++i) {
    }
    ok &= writeRegister(kRegIntStat, kIntStatServiced);

    // Enable key-event + GPI interrupts on the INT line.
    ok &= writeRegister(kRegCfg, static_cast<uint8_t>(kCfgKeIen | kCfgGpiIen));

    if (!ok) return false;  // config did not fully take — treated as an init failure by the caller.

    pinMode(kIntPin, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(kIntPin), onKeyInterrupt, FALLING);

    ready_ = true;
    return true;
}

size_t Tca8418Keyboard::drainRawEvents(uint8_t* out, size_t max) {
    if (!ready_) return 0;  // no bus.
    // Drain when EITHER the ISR flagged a falling edge OR the INT line is still asserted (active-low).
    // The level check is a local GPIO read, not I2C, so this stays off the bus until there is work — but
    // it makes the drain self-healing: if a previous ack write failed (a transient I2C error), or an edge
    // was missed, INT stays low and this re-drains and re-acks next poll instead of wedging entry for the
    // rest of the boot (a pure falling-edge trigger would never fire again on an already-low line). §3.
    if (!g_eventPending && digitalRead(kIntPin) != LOW) return 0;  // nothing pending — cheap exit.
    g_eventPending = false;

    // Drain every queued event this cycle (bounded by the queued count, the FIFO depth, and the caller's
    // buffer), so a burst of keys cannot leave a later event stuck behind them; INT is acked at the end
    // so the line de-asserts for the next event whether or not the caller cared about these bytes.
    const uint8_t queued = static_cast<uint8_t>(readRegister(kRegKeyLckEc) & 0x0F);
    size_t count = 0;
    for (uint8_t i = 0; i < queued && i < kFifoDrainLimit; ++i) {
        const uint8_t rawEvent = readRegister(kRegKeyEventA);
        if (rawEvent == 0) break;  // FIFO drained early.
        if (count < max) out[count++] = rawEvent;
    }
    writeRegister(kRegIntStat, kIntStatServiced);  // ack: let INT de-assert for the next event.
    return count;
}

bool Tca8418Keyboard::maintenanceKeyPressed() {
    // One FIFO-drain path, shared with the verify probe; the pure predicate decides. `found` is latched
    // across the whole batch (never an early return), so the drain always empties before we answer.
    uint8_t events[kFifoDrainLimit];
    const size_t count = drainRawEvents(events, kFifoDrainLimit);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (isMaintenanceKey(events[i])) found = true;
    }
    return found;
}

}  // namespace sapper

#endif  // UNIT_TEST
