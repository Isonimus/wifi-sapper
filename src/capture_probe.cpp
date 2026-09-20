/**
 * @file capture_probe.cpp
 * @brief Implementation of the slice-0032 bench capture-notification verify (ADR-0031). Device-only.
 */
#include "capture_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include "config/active_board.h"
#include "hal/display/display_hal.h"
#include "hunt_loop.h"
#include "net/provisioning_store.h"

namespace sapper {
namespace {

// Re-inject a capture well within the CAPTURED banner hold (kCapturedHoldMs = 2.5 s) so the banner is
// lit whenever the verify sends `dump`, and so the `[CAPTURE]` line repeats for a monitor that joins
// late (the S3 USB-CDC may miss a boot-once line — the slice-0028 lesson the verify gates around).
constexpr uint32_t kReinjectIntervalMs = 1500;
constexpr uint32_t kHeartbeatIntervalMs = 2000;
constexpr uint32_t kWideDrainIntervalMs = 3600000;  // 1 h: keep this local, no STA windows opened.

// A capture is announced at enqueue, before any drain, so this probe needs no network. A high drain
// threshold plus the wide time ceiling means the injected captures never open an STA window — the probe
// exercises only the enqueue→bus→surface path it is here to prove.
UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.drainThreshold = 1000;
    config.maxDrainIntervalMs = kWideDrainIntervalMs;
    return config;
}

bool g_active = false;
uint32_t g_lastReinjectMs = 0;
uint32_t g_lastHeartbeatMs = 0;

}  // namespace

bool captureProbeBegin(IDisplay& display) {
    const char* flag = SAPPER_TEST_CAPTURE;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    if (!kActiveBoard.hasDisplay) {
        // The serial `[CAPTURE]` half still works headless, but this verify also captures the panel
        // banner, so it wants a panel — fail loud rather than silently prove only half the claim.
        Serial.println("[FATAL] verify:capture-notify wants a board with a panel; this board has none");
        return false;
    }
    // main.cpp already began the panel and drew the bring-up frame; the surface renders over that same
    // canvas the serial dump streams back (one canvas), exactly like the screen probe.

    // Credentials are optional: the enqueue→announce path needs no network. Load whatever is present
    // (zeroed if none) purely to satisfy huntLoopBegin's seams.
    ProvisioningRecord creds = {};
    loadProvisioning(creds);

    if (!huntLoopBegin(creds, verifyConfig(), &display)) return false;  // huntLoopBegin logged the fault.
    huntLoopInjectStimulus();  // announce one capture right away so the first dump/line catches it.
    g_active = true;
    // Own status uses a distinct tag so the SerialEventLogger's `[CAPTURE] essid=…` line — the actual
    // proof the announcement fired — stays the unambiguous anchor the verify greps for.
    Serial.println("[CAPTURE-PROBE] hunt loop up, capture injected — send `dump` for the banner");
    return true;
}

bool captureProbeActive() { return g_active; }

void captureProbePump() {
    if (!g_active) return;

    huntLoopPump();  // renders the HUD + CAPTURED banner on each change; publishes any pending facts.

    const uint32_t now = millis();
    if (now - g_lastReinjectMs >= kReinjectIntervalMs) {
        g_lastReinjectMs = now;
        huntLoopInjectStimulus();  // re-announce so the banner stays lit and `[CAPTURE]` keeps repeating.
    }
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[CAPTURE-PROBE] waiting elapsed=%lums (send `dump` for the CAPTURED banner)\n",
                      static_cast<unsigned long>(now));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

#include "hal/display/display_hal.h"

namespace sapper {
bool captureProbeBegin(IDisplay&) { return false; }
bool captureProbeActive() { return false; }
void captureProbePump() {}
}  // namespace sapper

#endif
