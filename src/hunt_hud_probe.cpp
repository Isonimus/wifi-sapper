/**
 * @file hunt_hud_probe.cpp
 * @brief Implementation of the slice-0034 bench live-hunt-HUD verify (ADR-0033). Device-only.
 */
#include "hunt_hud_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include "config/active_board.h"
#include "hal/display/display_hal.h"
#include "hunt_loop.h"
#include "net/provisioning_store.h"

namespace sapper {
namespace {

// Inject the synthetic AP + handshake often enough that, once the engine is Capturing it, the collector
// stays populated across the capture window for whenever `dump` arrives. The engine's own discovery
// window (HuntConfig default ~4 s) gates how soon Capturing begins, so the probe simply keeps injecting.
constexpr uint32_t kInjectIntervalMs = 500;
constexpr uint32_t kHeartbeatIntervalMs = 2000;
constexpr uint32_t kWideDrainIntervalMs = 3600000;  // 1 h: keep this local, no STA windows opened.

// A capture-only probe: a high drain threshold + wide time ceiling means the injected captures never
// open an STA window, so the loop stays in hunt/render and needs no network.
UploadSupervisorConfig verifyConfig() {
    UploadSupervisorConfig config;
    config.drainThreshold = 1000;
    config.maxDrainIntervalMs = kWideDrainIntervalMs;
    return config;
}

bool g_active = false;
uint32_t g_lastInjectMs = 0;
uint32_t g_lastHeartbeatMs = 0;
bool g_announcedPopulated = false;

}  // namespace

bool huntHudProbeBegin(IDisplay& display) {
    const char* flag = SAPPER_TEST_HUNT_HUD;
    if (flag == nullptr || flag[0] == '\0') return false;  // inactive: normal boot runs.

    if (!kActiveBoard.hasDisplay) {
        Serial.println("[FATAL] verify:hunt-hud needs a board with a panel; this board has none");
        return false;
    }
    // main.cpp already began the panel; the surface renders over that same canvas the dump streams back.

    // Credentials are optional: the hunt+render path needs no network. Load whatever is present.
    ProvisioningRecord creds = {};
    loadProvisioning(creds);

    if (!huntLoopBegin(creds, verifyConfig(), &display)) return false;  // huntLoopBegin logged the fault.
    // Defer the cracked-sync so no STA window opens to stop the engine: on a network-less bench the
    // first sync is due at boot and a failed one stays due, which would churn WORKING/DEGRADED and never
    // let the hunt sustain Capturing (the collector would never populate). Done before the first pump.
    huntLoopDeferSync();
    huntLoopInjectHudStimulus();  // seed a synthetic AP right away so discovery has something to find.
    g_active = true;
    Serial.println("[HUNT-HUD] hunt loop up, injecting a synthetic AP + handshake — send `dump` for the HUD");
    return true;
}

bool huntHudProbeActive() { return g_active; }

void huntHudProbePump() {
    if (!g_active) return;

    huntLoopPump();  // renders the HUD (incl. the live-hunt line) on each change.

    const uint32_t now = millis();
    if (now - g_lastInjectMs >= kInjectIntervalMs) {
        g_lastInjectMs = now;
        huntLoopInjectHudStimulus();  // keep the discovered AP alive and re-land M1/M2 while Capturing.
    }

    // Announce once the live HUD is actually populated (Capturing with the injected Beacon + M1) — the
    // deterministic readiness signal the verify waits for before it captures the panel, plus a heartbeat.
    // The stimulus is a partial handshake (no M2) so the engine stays on the target; hence the gate is
    // Beacon + M1, not M2 (which would complete the capture and advance).
    const HuntSnapshot snap = huntLoopSnapshot();
    const bool populated = snap.phase == HuntPhase::Capturing && snap.hasBeacon && snap.hasM1;
    if (populated && !g_announcedPopulated) {
        g_announcedPopulated = true;
        Serial.printf("[HUNT-HUD] populated ssid='%s' B=%d M1=%d M2=%d M3=%d M4=%d — send `dump`\n",
                      snap.ssid, snap.hasBeacon, snap.hasM1, snap.hasM2, snap.hasM3, snap.hasM4);
    }
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[HUNT-HUD] waiting phase=%d collected=%u (send `dump` for the live HUD)\n",
                      static_cast<int>(snap.phase), static_cast<unsigned>(snap.collectedCount()));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the verify probe (§4 invariant #4).

#include "hal/display/display_hal.h"

namespace sapper {
bool huntHudProbeBegin(IDisplay&) { return false; }
bool huntHudProbeActive() { return false; }
void huntHudProbePump() {}
}  // namespace sapper

#endif
