/**
 * @file webhook_capture_probe.cpp
 * @brief Implementation of the slice-0036 bench capture-push verify probe (ADR-0035). Device-only.
 */
#include "webhook_capture_probe.h"

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

// The same snappy profile the webhook/LED/sync probes use: with no captures to upload, a due sync opens
// an STA window on its own at the short drain-time ceiling, so the enqueued notification flushes within
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

bool webhookCaptureProbeBegin() {
    const char* flag = SAPPER_TEST_WEBHOOK_CAPTURE;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    ProvisioningRecord creds = {};
    if (!loadProvisioning(creds)) {
        Serial.println("[FATAL] verify:webhook-capture needs real credentials + a wpa-sec key; none provisioned");
        return false;
    }
    if (!isUsableWebhookUrl(creds.webhookUrl)) {
        Serial.println("[FATAL] verify:webhook-capture needs a usable https webhook url provisioned "
                       "(set SAPPER_TEST_WEBHOOK_URL)");
        return false;
    }
    // Force capture-push on in RAM (test-hooks-only, §4 #4) so the proof holds regardless of the
    // provisioned checkbox: this verify proves the capture→POST path, not the operator's box state.
    creds.notifyCaptured = true;

    // The push rides real STA windows, so associate + sync the clock up front exactly as the sibling
    // webhook probe does — a bad network/clock surfaces here as a boot [FATAL], not a silent no-push.
    Serial.println("[WEBHOOK-CAPTURE] verify: associating + syncing clock before hunting");
    if (!connectStation(creds.ssid, creds.pass, nullptr) || !syncClock(nullptr)) {
        Serial.println("[FATAL] verify:webhook-capture could not associate or sync NTP");
        return false;
    }
    WiFi.disconnect(/*wifioff=*/false);

    if (!huntLoopBegin(creds, verifyConfig())) return false;  // huntLoopBegin already logged the fault.
    huntLoopInjectCaptureAlert();  // enqueue one synthetic capture notification onto the bus,
    huntLoopForceSyncDue();        // and arm a due sync so the next STA window flushes (POSTs) it.
    g_active = true;
    return true;
}

bool webhookCaptureProbeActive() { return g_active; }

void webhookCaptureProbePump() {
    if (!g_active) return;

    huntLoopPump();  // opens the STA window, which flushInWindow() uses to POST the enqueued capture.

    // On each successful send, re-inject a DISTINCT capture (huntLoopInjectCaptureAlert increments the
    // BSSID, so the per-BSSID dedup does not suppress it) and re-arm a due sync so the next STA window
    // POSTs it. This mirrors the sibling crack probe: a fresh POST recurs each cycle, so a monitor that
    // attaches late — or after this board's native USB-CDC drops delivery during a WiFi window — still
    // witnesses a `[WEBHOOK] sent code=` line and the `[WEBHOOK-CAPTURE] pushed` marker, rather than the
    // one-shot first send being the only evidence.
    const uint32_t sent = huntLoopWebhookSentCount();
    if (sent > g_lastSentCount) {
        g_lastSentCount = sent;
        Serial.printf("[WEBHOOK-CAPTURE] pushed count=%u\n", static_cast<unsigned>(sent));
        huntLoopInjectCaptureAlert();
        huntLoopForceSyncDue();
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[WEBHOOK-CAPTURE] waiting elapsed=%lums sent=%u\n",
                      static_cast<unsigned long>(now), static_cast<unsigned>(sent));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool webhookCaptureProbeBegin() { return false; }
bool webhookCaptureProbeActive() { return false; }
void webhookCaptureProbePump() {}
}  // namespace sapper

#endif
