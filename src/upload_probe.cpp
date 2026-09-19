/**
 * @file upload_probe.cpp
 * @brief Implementation of the slice-0018 bench upload-verify probe (ADR-0017). Device-only.
 */
#include "upload_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>
#include <WiFi.h>

#include "hunt_loop.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 2000;
// Snappy profile so the verify sees a drain promptly and REPEATEDLY: a single injected capture
// triggers it, and the backoff is capped low so the synthetic capture (always rejected, so never
// deleted) re-drains every few seconds — the observer can join at any time and still witness a full
// drain within its window, unlike the shipped loop's minutes-long backoff. The shipped loop keeps the
// wider defaults.
UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.drainThreshold = 1;
    config.backoffBaseMs = 3000;
    config.backoffCapMs = 8000;  // keep retries frequent so a drain is always observable.
    config.maxDrainIntervalMs = 60000;
    return config;
}

bool g_active = false;
bool g_stimulusInjected = false;
uint32_t g_lastDrainCount = 0;  // last drain cycle we have reported, so each new one prints once.
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool uploadProbeBegin() {
    const char* flag = SAPPER_TEST_UPLOAD;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    ProvisioningRecord creds = {};
    if (!loadProvisioning(creds)) {
        Serial.println("[FATAL] verify:upload needs real credentials + a wpa-sec key; none provisioned");
        return false;
    }

    // Associate + NTP up front so a TLS-valid clock exists before the first drain (the drain's own
    // bring-up does this too, but doing it here surfaces a bad network as a boot [FATAL], not a
    // mysterious deferred drain).
    Serial.println("[UPLOAD] verify: associating + syncing clock before hunting");
    if (!connectStation(creds.ssid, creds.pass, nullptr) || !syncClock(nullptr)) {
        Serial.println("[FATAL] verify:upload could not associate or sync NTP");
        return false;
    }
    WiFi.disconnect(/*wifioff=*/false);  // release the association; the hunt starts promiscuous.

    if (!huntLoopBegin(creds, verifyConfig())) return false;  // huntLoopBegin already logged the fault.
    g_active = true;
    return true;
}

bool uploadProbeActive() { return g_active; }

void uploadProbePump() {
    if (!g_active) return;

    // Inject the stimulus once, on the first pump, now that the loop is hunting.
    if (!g_stimulusInjected) {
        huntLoopInjectStimulus();
        g_stimulusInjected = true;
    }

    huntLoopPump();

    // Report every new drain (detected by the monotonic counter — outcomes can be identical), so an
    // observer that joins after the first drain still sees one within its window.
    const uint32_t drains = huntLoopDrainCount();
    if (drains > g_lastDrainCount) {
        g_lastDrainCount = drains;
        const DrainOutcome& drain = huntLoopLastDrain();
        Serial.printf("[UPLOAD] drain done associated=%d accepted=%u duplicate=%u rejected=%u "
                      "purged=%u storeErrors=%u resumeFailed=%d\n",
                      drain.associated, static_cast<unsigned>(drain.accepted),
                      static_cast<unsigned>(drain.duplicate), static_cast<unsigned>(drain.rejected),
                      static_cast<unsigned>(drain.purged), static_cast<unsigned>(drain.storeErrors),
                      drain.resumeFailed);
        if (!drain.associated) {
            Serial.println("[ERROR] drain could not associate — check WiFi/NTP");
        } else if (drain.accepted + drain.duplicate + drain.rejected == 0) {
            Serial.println("[ERROR] station associated but no upload was attempted");
        }
        // A capture accepted/duplicate is deleted, emptying the queue; re-inject so the next drain is
        // still observable (a real handshake then re-uploads as duplicate on a steady cadence). A
        // rejected synthetic stays queued and re-drains on its own, so this only fires for the real case.
        if (drain.accepted + drain.duplicate > 0) huntLoopInjectStimulus();
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[UPLOAD] waiting elapsed=%lums drains=%u\n", static_cast<unsigned long>(now),
                      static_cast<unsigned>(drains));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool uploadProbeBegin() { return false; }
bool uploadProbeActive() { return false; }
void uploadProbePump() {}
}  // namespace sapper

#endif
