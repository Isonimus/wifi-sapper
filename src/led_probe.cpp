/**
 * @file led_probe.cpp
 * @brief Implementation of the slice-0022 bench LED-verify probe (ADR-0021). Device-only.
 */
#include "led_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>
#include <WiFi.h>

#include "hunt_loop.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 2000;

// The same snappy profile the sync probe uses: with no captures to upload, a due sync opens an STA
// window on its own only at the drain time ceiling, so a short ceiling makes the LED cycle
// working → hunting every few seconds instead of every few minutes. The shipped loop keeps the wide
// defaults.
UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.drainThreshold = 8;
    config.maxDrainIntervalMs = 5000;
    config.backoffBaseMs = 5000;
    config.backoffCapMs = 15000;
    config.settleMs = 20;
    return config;
}

bool g_active = false;
uint32_t g_lastSyncCount = 0;
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool ledProbeBegin() {
    const char* flag = SAPPER_TEST_LED;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    ProvisioningRecord creds = {};
    if (!loadProvisioning(creds)) {
        Serial.println("[FATAL] verify:led needs real credentials + a wpa-sec key; none provisioned");
        return false;
    }

    // The LED cycle rides real STA windows, so associate + sync the clock up front exactly as the sync
    // probe does — a bad network/clock surfaces here as a boot [FATAL], not a mysterious quiet LED.
    Serial.println("[LED] verify: associating + syncing clock before hunting");
    if (!connectStation(creds.ssid, creds.pass, nullptr) || !syncClock(nullptr)) {
        Serial.println("[FATAL] verify:led could not associate or sync NTP");
        return false;
    }
    WiFi.disconnect(/*wifioff=*/false);

    if (!huntLoopBegin(creds, verifyConfig())) return false;  // huntLoopBegin already logged the fault.
    huntLoopForceSyncDue();        // arm a due sync so the next window drives working → hunting.
    huntLoopInjectCrackedAlert();  // and flash recovered right away, so it appears within seconds.
    g_active = true;
    return true;
}

bool ledProbeActive() { return g_active; }

void ledProbePump() {
    if (!g_active) return;

    huntLoopPump();  // drives the LED surface; the Esp32 driver logs each [LED] status= transition.

    // On each completed sync window (working → hunting), re-arm the next window and re-flash recovered,
    // so a monitor that joins late still witnesses every LED state.
    const uint32_t syncs = huntLoopSyncCount();
    if (syncs > g_lastSyncCount) {
        g_lastSyncCount = syncs;
        huntLoopInjectCrackedAlert();
        huntLoopForceSyncDue();
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[LED] waiting elapsed=%lums windows=%u\n", static_cast<unsigned long>(now),
                      static_cast<unsigned>(syncs));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool ledProbeBegin() { return false; }
bool ledProbeActive() { return false; }
void ledProbePump() {}
}  // namespace sapper

#endif
