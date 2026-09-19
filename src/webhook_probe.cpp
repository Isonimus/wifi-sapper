/**
 * @file webhook_probe.cpp
 * @brief Implementation of the slice-0024 bench webhook-verify probe (ADR-0023). Device-only.
 */
#include "webhook_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>
#include <WiFi.h>

#include "hunt_loop.h"
#include "net/provisioning.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 2000;

// The same snappy profile the LED/sync probes use: with no captures to upload, a due sync opens an STA
// window on its own at the short drain time ceiling, so the enqueued notification flushes within
// seconds instead of at the shipped multi-minute ceiling. The shipped loop keeps the wide defaults.
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
uint32_t g_lastSentCount = 0;
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool webhookProbeBegin() {
    const char* flag = SAPPER_TEST_WEBHOOK;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    ProvisioningRecord creds = {};
    if (!loadProvisioning(creds)) {
        Serial.println("[FATAL] verify:webhook needs real credentials + a wpa-sec key; none provisioned");
        return false;
    }
    if (!isUsableWebhookUrl(creds.webhookUrl)) {
        Serial.println("[FATAL] verify:webhook needs a usable https webhook url provisioned "
                       "(set SAPPER_TEST_WEBHOOK_URL)");
        return false;
    }

    // The push rides real STA windows, so associate + sync the clock up front exactly as the LED/sync
    // probes do — a bad network/clock surfaces here as a boot [FATAL], not a mysteriously silent push.
    Serial.println("[WEBHOOK] verify: associating + syncing clock before hunting");
    if (!connectStation(creds.ssid, creds.pass, nullptr) || !syncClock(nullptr)) {
        Serial.println("[FATAL] verify:webhook could not associate or sync NTP");
        return false;
    }
    WiFi.disconnect(/*wifioff=*/false);

    if (!huntLoopBegin(creds, verifyConfig())) return false;  // huntLoopBegin already logged the fault.
    huntLoopInjectCrackedAlert();  // enqueue one synthetic new-password notification onto the bus,
    huntLoopForceSyncDue();        // and arm a due sync so the next STA window flushes (POSTs) it.
    g_active = true;
    return true;
}

bool webhookProbeActive() { return g_active; }

void webhookProbePump() {
    if (!g_active) return;

    huntLoopPump();  // opens the STA window, which flushInWindow() uses to POST the enqueued alert.

    // On each successful send, re-inject and re-arm so a monitor that joins late still witnesses a POST.
    const uint32_t sent = huntLoopWebhookSentCount();
    if (sent > g_lastSentCount) {
        g_lastSentCount = sent;
        huntLoopInjectCrackedAlert();
        huntLoopForceSyncDue();
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[WEBHOOK] waiting elapsed=%lums sent=%u\n", static_cast<unsigned long>(now),
                      static_cast<unsigned>(sent));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool webhookProbeBegin() { return false; }
bool webhookProbeActive() { return false; }
void webhookProbePump() {}
}  // namespace sapper

#endif
