/**
 * @file hunt_probe.cpp
 * @brief Implementation of the slice-0016 bench hunt-loop probe (ADR-0015). Device-only.
 */
#include "hunt_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include <cstdlib>

#include "net/ap_registry.h"
#include "net/channel_list_arg.h"
#include "net/hunt_engine.h"
#include "net/radio_sniffer_esp32.h"

namespace sapper {
namespace {

constexpr uint32_t kDefaultDwellMs = 300;  // ≈3× the 102.4ms beacon interval (ADR-0013 decision #5).
constexpr uint32_t kHeartbeatIntervalMs = 2000;
// The probe runs tighter windows than the appliance defaults so a few-AP environment cycles
// discover→capture→discover well within the verify's budget; the loop mechanics are identical
// (ADR-0015). The shipped appliance keeps HuntConfig's wider defaults.
constexpr uint32_t kProbeDiscoverMs = 3000;
constexpr uint32_t kProbeCaptureMs = 3000;

/// Prints each wpa-sec-valid capture the engine reports. On air, without a deauth to force a
/// renegotiation, this rarely fires — the probe proves the loop mechanics, not a live capture
/// (slice-5, LEDGER) — but the seam is wired exactly as the uploader will attach to it.
class PrintCaptureObserver : public CaptureReadyObserver {
public:
    void onCaptureReady(const CapturedHandshake& handshake) override {
        Serial.printf("[HUNT] captured bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%s channel=%u\n",
                      handshake.bssid[0], handshake.bssid[1], handshake.bssid[2], handshake.bssid[3],
                      handshake.bssid[4], handshake.bssid[5], handshake.ssid, handshake.channel);
    }
};

const char* phaseName(HuntEngine::Phase phase) {
    switch (phase) {
        case HuntEngine::Phase::Idle: return "idle";
        case HuntEngine::Phase::Discovering: return "discovering";
        case HuntEngine::Phase::Capturing: return "capturing";
        case HuntEngine::Phase::Quiescing: return "quiescing";
    }
    return "unknown";
}

bool g_active = false;
HuntEngine* g_engine = nullptr;
ApRegistry* g_registry = nullptr;
const char* g_hopText = nullptr;
uint32_t g_dwellMs = kDefaultDwellMs;
HuntEngine::Phase g_lastPhase = HuntEngine::Phase::Idle;
size_t g_reported = 0;
bool g_overflowReported = false;
uint32_t g_lastHeartbeatMs = 0;

void reportDiscoveredTargets() {
    // count() publishes the append-only table, so entries [g_reported, count) are fully written and
    // safe to read from this app-task pump (ADR-0013/ADR-0015).
    const size_t discovered = g_registry->count();
    for (; g_reported < discovered; ++g_reported) {
        const DiscoveredAp& ap = g_registry->at(g_reported);
        Serial.printf("[HUNT] target bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%s channel=%u\n",
                      ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
                      ap.ssid, ap.channel);
    }
    if (g_registry->overflowed() && !g_overflowReported) {
        g_overflowReported = true;
        Serial.printf("[ERROR] AP registry overflowed at %u entries; enumeration incomplete\n",
                      static_cast<unsigned>(kMaxDiscoveredAps));
    }
}

void announcePhase(HuntEngine::Phase phase) {
    switch (phase) {
        case HuntEngine::Phase::Discovering:
            g_reported = 0;  // the engine reset the registry for a new sweep.
            g_overflowReported = false;
            Serial.printf("[HUNT] discovering channels=%s dwell=%lums\n", g_hopText,
                          static_cast<unsigned long>(g_dwellMs));
            return;
        case HuntEngine::Phase::Capturing: {
            const uint8_t* bssid = g_engine->capturingBssid();
            if (bssid != nullptr) {
                Serial.printf("[HUNT] capturing bssid=%02x:%02x:%02x:%02x:%02x:%02x channel=%u\n",
                              bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                              g_engine->parkedChannel());
            }
            return;
        }
        case HuntEngine::Phase::Idle:
        case HuntEngine::Phase::Quiescing:
            return;  // internal transitions; not part of the observable loop the verify checks.
    }
}

}  // namespace

bool huntProbeBegin() {
    const char* hopText = SAPPER_TEST_HUNT;
    if (hopText == nullptr || hopText[0] == '\0') return false;  // inactive: normal boot runs.

    uint8_t channels[kMaxHopChannels] = {0};
    size_t count = 0;
    if (!parseChannelListArg(hopText, channels, count)) {
        Serial.printf("[FATAL] SAPPER_TEST_HUNT not a channel list in %u-%u: '%s'\n", kMinHopChannel,
                      kMaxHopChannel, hopText);
        return false;
    }

    // Dwell is optional: empty falls back to the spec-grounded default; a non-empty but malformed
    // value fails loud rather than silently sweeping too fast to catch a beacon (ADR-0013).
    uint32_t dwellMs = kDefaultDwellMs;
    const char* dwellText = SAPPER_TEST_HUNT_DWELL_MS;
    if (dwellText != nullptr && dwellText[0] != '\0') {
        char* end = nullptr;
        const long parsed = std::strtol(dwellText, &end, 10);
        if (end == dwellText || *end != '\0' || parsed <= 0) {
            Serial.printf("[FATAL] SAPPER_TEST_HUNT_DWELL_MS not a positive integer: '%s'\n", dwellText);
            return false;
        }
        dwellMs = static_cast<uint32_t>(parsed);
    }

    HuntConfig config;
    config.dwellMs = dwellMs;
    config.discoverWindowMs = kProbeDiscoverMs;
    config.captureWindowMs = kProbeCaptureMs;

    // File-scope lifetime: the engine is the sniffer's consumer for the probe's whole run and pump()
    // drives tick() each loop. This is the real capture spine (ADR-0015): Esp32RadioSniffer +
    // ApRegistry, orchestrated by the HuntEngine, capture-ready wired to a print observer.
    static Esp32RadioSniffer sniffer;
    static ApRegistry registry;
    static PrintCaptureObserver observer;
    static HuntEngine engine(sniffer, registry, observer, channels, count, config);
    g_engine = &engine;
    g_registry = &registry;
    g_hopText = hopText;
    g_dwellMs = dwellMs;

    if (!engine.begin(millis())) {
        Serial.println("[FATAL] hunt engine could not begin (promiscuous mode failed)");
        return false;
    }
    g_active = true;
    g_lastPhase = engine.phase();
    Serial.printf("[HUNT] discovering channels=%s dwell=%lums\n", hopText,
                  static_cast<unsigned long>(dwellMs));
    return true;
}

bool huntProbeActive() { return g_active; }

void huntProbePump() {
    if (!g_active || g_engine == nullptr || g_registry == nullptr) return;

    g_engine->tick(millis());

    const HuntEngine::Phase phase = g_engine->phase();
    if (phase != g_lastPhase) {
        announcePhase(phase);
        g_lastPhase = phase;
    }
    if (phase == HuntEngine::Phase::Discovering) reportDiscoveredTargets();

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        // Liveness for the artifact: distinguishes "device alive, hunting" from "device hung".
        Serial.printf("[HUNT] waiting phase=%s elapsed=%lums discovered=%u\n", phaseName(phase),
                      static_cast<unsigned long>(now), static_cast<unsigned>(g_registry->count()));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary hunts (ADR-0015 decision #8).

namespace sapper {
bool huntProbeBegin() { return false; }
bool huntProbeActive() { return false; }
void huntProbePump() {}
}  // namespace sapper

#endif
