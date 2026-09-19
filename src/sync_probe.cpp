/**
 * @file sync_probe.cpp
 * @brief Implementation of the slice-0020 bench sync-verify probe (ADR-0019). Device-only.
 */
#include "sync_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>
#include <WiFi.h>

#include "hunt_loop.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 2000;

// Snappy profile so the verify sees a sync-only STA window promptly and repeatedly: with no captures to
// upload, a due sync opens a window on its own only at the drain time ceiling (upload_supervisor.cpp), so
// a short ceiling makes the first sync fire seconds after boot instead of minutes. A late-joining
// observer still witnesses a full sync within its window. The shipped loop keeps the wide defaults.
UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.drainThreshold = 8;         // no captures here; keep the upload threshold out of the way.
    config.maxDrainIntervalMs = 5000;  // the forced sync-only window's rate limit — fire ~5s after arming.
    config.backoffBaseMs = 5000;
    config.backoffCapMs = 15000;       // keep retries frequent so a sync is always observable.
    config.settleMs = 20;
    return config;
}

bool g_active = false;
uint32_t g_lastSyncCount = 0;   // last sync we have reported, so each new one prints once.
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool syncProbeBegin() {
    const char* flag = SAPPER_TEST_SYNC;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    ProvisioningRecord creds = {};
    if (!loadProvisioning(creds)) {
        Serial.println("[FATAL] verify:sync needs real credentials + a wpa-sec key; none provisioned");
        return false;
    }

    // Associate + NTP up front so a bad network/clock surfaces as a boot [FATAL] here rather than a
    // mysterious deferred sync (the drain's own bring-up would do this too — same reasoning as the
    // upload probe). The pinned-cert download cannot validate against a 1970 clock (ADR-0017 decision #1).
    Serial.println("[SYNC] verify: associating + syncing clock before hunting");
    if (!connectStation(creds.ssid, creds.pass, nullptr) || !syncClock(nullptr)) {
        Serial.println("[FATAL] verify:sync could not associate or sync NTP");
        return false;
    }
    WiFi.disconnect(/*wifioff=*/false);  // release the association; the hunt starts promiscuous.

    if (!huntLoopBegin(creds, verifyConfig())) return false;  // huntLoopBegin already logged the fault.
    huntLoopForceSyncDue();  // arm a due sync so the next STA window runs it (the clock-advance stimulus).
    g_active = true;
    return true;
}

bool syncProbeActive() { return g_active; }

void syncProbePump() {
    if (!g_active) return;

    huntLoopPump();

    // Report every new sync (detected by the monotonic counter — outcomes can be identical), so an
    // observer that joins late still sees one within its window.
    const uint32_t syncs = huntLoopSyncCount();
    if (syncs > g_lastSyncCount) {
        g_lastSyncCount = syncs;
        const SyncOutcome& s = huntLoopLastSync();
        if (!s.ok) {
            // A failed sync is loud so the verify's error-check half fails: no seed, no proof of the path.
            Serial.printf("[ERROR] sync failed fetch=%d storeError=%d downloaded=%u malformed=%u\n",
                          static_cast<int>(s.fetch), s.storeError, static_cast<unsigned>(s.downloaded),
                          static_cast<unsigned>(s.malformed));
        } else {
            Serial.printf("[SYNC] verify sync ok first=%d downloaded=%u new=%u malformed=%u overflow=%u\n",
                          s.firstSync, static_cast<unsigned>(s.downloaded),
                          static_cast<unsigned>(s.newPasswords), static_cast<unsigned>(s.malformed),
                          static_cast<unsigned>(s.overflow));
        }
        // Re-arm so the sync re-runs on the next ceiling: a steady cadence keeps it observable for a
        // late-joining monitor, exactly as the upload probe re-injects to keep drains observable.
        huntLoopForceSyncDue();
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[SYNC] waiting elapsed=%lums syncs=%u\n", static_cast<unsigned long>(now),
                      static_cast<unsigned>(syncs));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

namespace sapper {
bool syncProbeBegin() { return false; }
bool syncProbeActive() { return false; }
void syncProbePump() {}
}  // namespace sapper

#endif
