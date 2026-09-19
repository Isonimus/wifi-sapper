/**
 * @file screen_probe.cpp
 * @brief Implementation of the slice-0026 bench screen-verify probe (ADR-0025). Device-only.
 */
#include "screen_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include "config/active_board.h"
#include "hal/display/display_hal.h"
#include "hunt_loop.h"
#include "net/provisioning_store.h"

namespace sapper {
namespace {

// Re-arm the banner well within its hold (kToastHoldMs = 6 s) so it is lit whenever the verify sends
// `dump`. The wide drain interval suppresses capture/time-triggered windows, but the first hourly sync
// still opens one at boot; with no network it cannot associate, so the HUD honestly reads DEGRADED —
// which is fine, since this probe proves the panel RENDERS (every HUD field + the banner), not any
// particular status. All colour paths render the same way.
constexpr uint32_t kReinjectIntervalMs = 3000;
constexpr uint32_t kHeartbeatIntervalMs = 2000;
constexpr uint32_t kWideDrainIntervalMs = 3600000;  // 1 h: suppress capture/time-triggered drains.

UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.maxDrainIntervalMs = kWideDrainIntervalMs;
    return config;
}

bool g_active = false;
uint32_t g_lastReinjectMs = 0;
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool screenProbeBegin(IDisplay& display) {
    const char* flag = SAPPER_TEST_SCREEN;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    if (!kActiveBoard.hasDisplay) {
        Serial.println("[FATAL] verify:screen needs a board with a panel; this board has none");
        return false;
    }
    // main.cpp already began the panel and drew the bring-up frame before dispatching probes, so the
    // display is live here — the surface renders over that same canvas the serial dump streams back.

    // Credentials are optional here: the render path needs no network, so a board with no provisioning
    // still renders. Load whatever is present (zeroed if none) purely to satisfy huntLoopBegin's seams.
    ProvisioningRecord creds = {};
    loadProvisioning(creds);

    if (!huntLoopBegin(creds, verifyConfig(), &display)) return false;  // huntLoopBegin logged the fault.
    huntLoopInjectCrackedAlert();  // arm the CRACKED banner right away, so the first dump catches it.
    g_active = true;
    Serial.println("[SCREEN] verify: HUD up, CRACKED banner armed — send `dump` to capture");
    return true;
}

bool screenProbeActive() { return g_active; }

void screenProbePump() {
    if (!g_active) return;

    huntLoopPump();  // renders the HUD + banner (Esp32ScreenRenderer) on each status/heartbeat change.

    const uint32_t now = millis();
    if (now - g_lastReinjectMs >= kReinjectIntervalMs) {
        g_lastReinjectMs = now;
        huntLoopInjectCrackedAlert();  // keep the banner lit across its hold for whenever `dump` arrives.
    }
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[SCREEN] waiting elapsed=%lums (send `dump` to capture the HUD + banner)\n",
                      static_cast<unsigned long>(now));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

#include "hal/display/display_hal.h"

namespace sapper {
bool screenProbeBegin(IDisplay&) { return false; }
bool screenProbeActive() { return false; }
void screenProbePump() {}
}  // namespace sapper

#endif
