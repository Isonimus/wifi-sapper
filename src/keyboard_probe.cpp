/**
 * @file keyboard_probe.cpp
 * @brief Implementation of the ADV keyboard-keycode bench probe (ADR-0057). Device-only.
 */
#include "keyboard_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include "hal/input/tca8418_event.h"
#include "hal/input/tca8418_keyboard.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 3000;
constexpr size_t kMaxEventsPerPump = 16;

bool g_active = false;
Tca8418Keyboard g_keyboard;
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool keyboardProbeBegin() {
    const char* flag = SAPPER_TEST_KB;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    if (!g_keyboard.begin()) {
        // Fail loud: the verify watches for [FATAL] and cannot confirm the keycode without the controller.
        Serial.println("[FATAL] verify:keyboard — TCA8418 did not ACK; is this a Cardputer ADV?");
        return false;
    }
    g_active = true;
    Serial.printf("[KB] probe up — press keys to read their decoded number; 'm' should report code=%u\n",
                  static_cast<unsigned>(kMaintenanceKeycode));
    return true;
}

bool keyboardProbeActive() { return g_active; }

void keyboardProbePump() {
    if (!g_active) return;

    uint8_t events[kMaxEventsPerPump];
    const size_t count = g_keyboard.drainRawEvents(events, kMaxEventsPerPump);
    for (size_t i = 0; i < count; ++i) {
        const KeyEvent event = decodeKeyEvent(events[i]);
        Serial.printf("[KB] raw=0x%02X code=%u pressed=%u row=%u col=%u%s\n", events[i],
                      static_cast<unsigned>(event.keycode), event.pressed ? 1u : 0u,
                      static_cast<unsigned>(event.row), static_cast<unsigned>(event.col),
                      isMaintenanceKey(events[i]) ? "  <- MAINTENANCE KEY" : "");
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.println("[KB] waiting — press 'm' to confirm its keycode");
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool keyboardProbeBegin() { return false; }
bool keyboardProbeActive() { return false; }
void keyboardProbePump() {}
}  // namespace sapper

#endif
