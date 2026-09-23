/**
 * @file rssi_probe.cpp
 * @brief RSSI↔capture-correlation probe (ADR-0067). Device-only, SAPPER_TEST_HOOKS-gated.
 *
 * Runs the real, unmodified HuntEngine in discovery order (armed, so captures actually fire) while a
 * throwaway RssiTapSniffer records each beacon's rx_ctrl.rssi per BSSID out-of-band. The production
 * RadioSniffer seam is not touched — §4 #8 (raw bytes only) holds; the RSSI never reaches the engine. The
 * engine wires NO cracked/submitted gate (no setCrackedLookup/setSubmittedLookup) and never uploads, so
 * it targets and attempts EVERY encrypted AP regardless of wpa-sec state — a lab whose handshakes are all
 * already uploaded needs no memory wipe, and "captured" is a pure on-air capture event.
 *
 * The multi-hour run is untethered (no serial visible), so each resolved capture attempt is accumulated
 * into a compact per-band histogram (net/rssi_histogram.h) persisted to LittleFS, and the histogram is
 * dumped to serial at boot when the board is reattached. A run tag (SAPPER_TEST_RSSI_RUN) starts the
 * counters fresh when it differs from the persisted file and resumes them when it matches. Both the file
 * and the dump are identity-free (per-band counts only), so §4 #25 holds by construction. A per-attempt
 * `[RSSI] rssi=…` row is still printed for the tethered smoke test.
 */
#include "rssi_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <atomic>
#include <cstring>

#include "net/ap_registry.h"
#include "net/channel_list_arg.h"
#include "net/hunt_engine.h"
#include "net/radio_sniffer.h"
#include "net/raw_transmitter_esp32.h"
#include "net/rssi_histogram.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 5000;
/// Beacon management-frame first octet (type=management, subtype=beacon): the only frames we tap for
/// RSSI, matching the beacon-based discovery an RSSI scorer would order on (ADR-0013).
constexpr uint8_t kBeaconFrameControl0 = 0x80;
/// A management header is 24 bytes; addr3 (the BSSID) sits at offset 16. Guard before reading it.
constexpr size_t kMgmtHeaderLen = 24;
constexpr size_t kBssidOffset = 16;

constexpr const char* kHistPath = "/rssi-probe.bin";
constexpr const char* kHistTempPath = "/rssi-probe.tmp";
constexpr const char* kDefaultRunTag = "default";

// ---- The out-of-band RSSI tap (probe-only; the production seam is untouched). ----

/// One BSSID's most-recent beacon RSSI. Append-only, first-sighting-wins: a reader sees a fully-written
/// bssid (published by the release store to g_rssiCount) and reads the latest rssi. Unlike ApRegistry —
/// whose entries are immutable once published — this entry's rssi is deliberately mutable, a single-byte
/// atomic overwritten in place (latest-wins), because a probe wants each BSSID's most recent signal.
struct RssiEntry {
    uint8_t bssid[6] = {0};
    std::atomic<int8_t> rssi{kRssiUnknown};
};

RssiEntry g_rssi[kMaxDiscoveredAps];
std::atomic<size_t> g_rssiCount{0};
std::atomic<bool> g_rssiOverflow{false};
FrameConsumer* g_tapConsumer = nullptr;  // set on the app task before the callback can fire; read-only after.

bool bssidEq(const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, 6) == 0; }

/// Driver-task context (like the production RX callback): record @p rssi for @p bssid, append-only.
void recordRssi(const uint8_t* bssid, int8_t rssi) {
    const size_t count = g_rssiCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (bssidEq(g_rssi[i].bssid, bssid)) {
            g_rssi[i].rssi.store(rssi, std::memory_order_relaxed);  // single-byte, latest-wins.
            return;
        }
    }
    if (count >= kMaxDiscoveredAps) {
        g_rssiOverflow.store(true, std::memory_order_relaxed);
        return;
    }
    std::memcpy(g_rssi[count].bssid, bssid, 6);           // write the identity first,
    g_rssi[count].rssi.store(rssi, std::memory_order_relaxed);
    g_rssiCount.store(count + 1, std::memory_order_release);  // then publish it.
}

/// App-task context: the last beacon RSSI recorded for @p bssid, or kRssiUnknown if none.
int8_t rssiFor(const uint8_t* bssid) {
    const size_t count = g_rssiCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (bssidEq(g_rssi[i].bssid, bssid)) return g_rssi[i].rssi.load(std::memory_order_relaxed);
    }
    return kRssiUnknown;
}

/// The ESP-IDF promiscuous RX callback: read rx_ctrl.rssi, tap it for beacons, then forward the raw
/// frame bytes to the engine exactly as Esp32RadioSniffer does (no protocol logic added to the seam; the
/// RSSI tap is probe-only and never reaches the consumer). Runs in the Wi-Fi driver task — bounded, no
/// allocation, no blocking (§4 #10).
void onTapRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;  // MISC sig_len is not a frame length.
    if (g_tapConsumer == nullptr || buf == nullptr) return;
    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint16_t len = static_cast<uint16_t>(pkt->rx_ctrl.sig_len);
    if (type == WIFI_PKT_MGMT && len >= kMgmtHeaderLen && pkt->payload[0] == kBeaconFrameControl0) {
        recordRssi(pkt->payload + kBssidOffset, static_cast<int8_t>(pkt->rx_ctrl.rssi));
    }
    g_tapConsumer->onFrame(pkt->payload, len);  // raw bytes to the engine — the production contract.
}

/// A throwaway RadioSniffer clone of Esp32RadioSniffer that installs the RSSI-tapping callback instead of
/// the raw-only one. Probe-only; the production seam (radio_sniffer_esp32.cpp) is unchanged.
class RssiTapSniffer : public RadioSniffer {
public:
    bool begin(uint8_t channel, FrameConsumer& consumer) override {
        g_tapConsumer = &consumer;
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        const wifi_promiscuous_filter_t filter = {
            .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
        };
        if (esp_wifi_set_promiscuous_rx_cb(&onTapRx) == ESP_OK &&
            esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
            esp_wifi_set_promiscuous(true) == ESP_OK &&
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
            return true;
        }
        stop();
        return false;
    }
    bool setChannel(uint8_t channel) override {
        return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
    }
    void stop() override {
        esp_wifi_set_promiscuous(false);
        g_tapConsumer = nullptr;
    }
};

// ---- Persisted histogram (LittleFS, temp-then-rename — the submitted/cracked store idiom). ----

RssiHistogram g_hist;

/// Load the persisted histogram into @p hist. Returns false if there is no valid prior file (a fresh
/// run), so the caller resets it for the current tag.
bool loadHistogram(RssiHistogram& hist) {
    if (!LittleFS.exists(kHistPath)) return false;
    File f = LittleFS.open(kHistPath, "r");
    if (!f) {
        Serial.println("[ERROR] rssi histogram open failed");
        return false;
    }
    uint8_t buf[kRssiHistogramBytes];
    const size_t read = f.read(buf, sizeof(buf));
    f.close();
    if (read != sizeof(buf)) {
        Serial.println("[ERROR] rssi histogram short read");
        return false;
    }
    return deserializeHistogram(buf, sizeof(buf), hist);
}

/// Persist @p hist by temp-then-rename so a power loss mid-write never destroys the prior file (the exact
/// idiom LittleFsSubmittedStore::save uses). Returns whether it landed.
bool saveHistogram(const RssiHistogram& hist) {
    uint8_t buf[kRssiHistogramBytes];
    if (serializeHistogram(hist, buf, sizeof(buf)) != sizeof(buf)) {
        Serial.println("[ERROR] rssi histogram serialize failed");
        return false;
    }
    File f = LittleFS.open(kHistTempPath, "w");
    if (!f) {
        Serial.println("[ERROR] rssi histogram temp open failed");
        return false;
    }
    const size_t wrote = f.write(buf, sizeof(buf));
    f.close();
    if (wrote != sizeof(buf)) {
        Serial.println("[ERROR] rssi histogram temp write failed");
        LittleFS.remove(kHistTempPath);
        return false;
    }
    if (!LittleFS.rename(kHistTempPath, kHistPath)) {
        Serial.println("[ERROR] rssi histogram rename failed");
        LittleFS.remove(kHistTempPath);
        return false;
    }
    return true;
}

const char* kBandLabels[kRssiBandCount] = {"ge-50", "-50to-60", "-60to-70", "-70to-80", "lt-80"};

/// Print the accumulated histogram as identity-free, parseable lines — this is the readout on reattach.
void dumpHistogram(const RssiHistogram& hist) {
    uint32_t totalAttempts = hist.naAttempts;
    uint32_t totalCaptured = hist.naCaptured;
    for (size_t i = 0; i < kRssiBandCount; ++i) {
        totalAttempts += hist.attempts[i];
        totalCaptured += hist.captured[i];
    }
    Serial.printf("[RSSI] histogram tag=%s total_attempts=%lu total_captured=%lu\n", hist.runTag,
                  static_cast<unsigned long>(totalAttempts), static_cast<unsigned long>(totalCaptured));
    for (size_t i = 0; i < kRssiBandCount; ++i) {
        Serial.printf("[RSSI] band=%u label=%s attempts=%lu captured=%lu\n", static_cast<unsigned>(i),
                      kBandLabels[i], static_cast<unsigned long>(hist.attempts[i]),
                      static_cast<unsigned long>(hist.captured[i]));
    }
    Serial.printf("[RSSI] band=na attempts=%lu captured=%lu\n",
                  static_cast<unsigned long>(hist.naAttempts),
                  static_cast<unsigned long>(hist.naCaptured));
    Serial.println("[RSSI] histogram end");
}

// ---- Capture-attempt pairing (app task only: pump + onCaptureReady, both under engine.tick). ----

/// Latches the current capture attempt so the pump can pair it with an outcome.
struct Attempt {
    bool active = false;
    uint8_t bssid[6] = {0};
    uint32_t startMs = 0;
    uint8_t channel = 0;
    int8_t rssi = kRssiUnknown;
};

Attempt g_attempt;
uint32_t g_attempts = 0;
uint32_t g_captured = 0;
uint32_t g_flushFailures = 0;  // persisted-histogram flush failures, surfaced in the heartbeat (fail loud).

/// Record one resolved attempt: accumulate it into the persisted histogram, flush, and (for the tethered
/// smoke test) print an identity-free row.
void resolveAttempt(const Attempt& a, uint32_t nowMs, bool captured) {
    ++g_attempts;
    if (captured) ++g_captured;
    recordAttempt(g_hist, a.rssi, captured);
    // Flush every attempt: the run is untethered and may lose power, so at most the current attempt is
    // ever at risk. The file is tiny (kRssiHistogramBytes) and written far less than once a second. A
    // failure keeps the RAM counts intact (the next flush may land) but is counted and surfaced in the
    // heartbeat — a silently non-persisting run would look empty on reattach with no clue why.
    if (!saveHistogram(g_hist)) ++g_flushFailures;
    if (a.rssi == kRssiUnknown) {
        Serial.printf("[RSSI] rssi=na channel=%u ms=%lu outcome=%s\n", a.channel,
                      static_cast<unsigned long>(nowMs - a.startMs), captured ? "captured" : "empty");
    } else {
        Serial.printf("[RSSI] rssi=%d channel=%u ms=%lu outcome=%s\n", a.rssi, a.channel,
                      static_cast<unsigned long>(nowMs - a.startMs), captured ? "captured" : "empty");
    }
}

/// Reports each wpa-sec-valid capture as the "captured" outcome for the attempt in flight.
class CorrelationObserver : public CaptureReadyObserver {
public:
    void onCaptureReady(const CapturedHandshake& handshake) override {
        if (g_attempt.active && bssidEq(g_attempt.bssid, handshake.bssid)) {
            resolveAttempt(g_attempt, millis(), /*captured=*/true);
            g_attempt.active = false;
        }
    }
};

bool g_active = false;
HuntEngine* g_engine = nullptr;
HuntEngine::Phase g_lastPhase = HuntEngine::Phase::Idle;
uint32_t g_lastHeartbeatMs = 0;

/// Close out any in-flight attempt as empty — a target whose window ended without a valid handshake.
void resolvePendingEmpty(uint32_t nowMs) {
    if (g_attempt.active) {
        resolveAttempt(g_attempt, nowMs, /*captured=*/false);
        g_attempt.active = false;
    }
}

/// Begin tracking the target the engine just entered Capturing for.
void latchAttempt(uint32_t nowMs) {
    const uint8_t* bssid = g_engine->capturingBssid();
    if (bssid == nullptr) return;  // defensive: capturingBssid() is non-null in Capturing.
    resolvePendingEmpty(nowMs);    // the previous target timed out without a capture.
    g_attempt.active = true;
    std::memcpy(g_attempt.bssid, bssid, 6);
    g_attempt.startMs = nowMs;
    g_attempt.channel = g_engine->parkedChannel();
    g_attempt.rssi = rssiFor(bssid);
}

}  // namespace

bool rssiProbeBegin() {
    const char* hopText = SAPPER_TEST_RSSI;
    if (hopText == nullptr || hopText[0] == '\0') return false;  // inactive: normal boot runs.

    uint8_t channels[kMaxHopChannels] = {0};
    size_t count = 0;
    if (!parseChannelListArg(hopText, channels, count)) {
        Serial.printf("[FATAL] SAPPER_TEST_RSSI not a channel list in %u-%u: '%s'\n", kMinHopChannel,
                      kMaxHopChannel, hopText);
        return false;
    }

    const char* runTag = SAPPER_TEST_RSSI_RUN;
    if (runTag == nullptr || runTag[0] == '\0') runTag = kDefaultRunTag;
    // Fail loud rather than truncate: a tag stored truncated would let two runs whose tags differ only
    // past the limit resume into one histogram silently (ADR-0067 review). Reject it so the operator picks
    // a tag that fits — never a silent merge.
    if (!runTagFits(runTag)) {
        Serial.printf("[FATAL] SAPPER_TEST_RSSI_RUN too long (max %u chars): '%s'\n",
                      static_cast<unsigned>(kRssiRunTagLen - 1), runTag);
        return false;
    }

    if (!LittleFS.begin(/*formatOnFail=*/true)) {
        Serial.println("[FATAL] rssi probe could not mount LittleFS");
        return false;
    }
    // Resume the persisted run only if its tag matches this build's exactly; otherwise start a fresh run.
    // A field power-cycle keeps the tag and resumes; bumping SAPPER_TEST_RSSI_RUN + reflashing begins clean.
    if (loadHistogram(g_hist) && histogramHasTag(g_hist, runTag)) {
        Serial.printf("[RSSI] resumed run tag=%s\n", g_hist.runTag);
    } else {
        resetForTag(g_hist, runTag);
        if (!saveHistogram(g_hist)) {
            Serial.println("[FATAL] rssi probe could not persist a fresh histogram");
            return false;
        }
        Serial.printf("[RSSI] started fresh run tag=%s\n", g_hist.runTag);
    }
    dumpHistogram(g_hist);  // the readout on reattach: the accumulated table so far.

    // Shipped HuntConfig defaults on purpose: the correlation must reflect the real capture behaviour
    // (8 s windows, the shipped deauth cadence), not a probe-tightened variant. Armed with a real
    // transmitter so captures actually fire — an unarmed hunt almost never completes a handshake, which
    // would leave the "captured" class empty and the correlation unmeasurable. No cracked/submitted gate
    // is wired, so every encrypted AP is attempted. Deauth only your own authorized networks (§4 #16).
    static Esp32RawTransmitter transmitter;
    static RssiTapSniffer sniffer;
    static ApRegistry registry;
    static CorrelationObserver observer;
    static HuntEngine engine(sniffer, registry, observer, channels, count, HuntConfig{}, &transmitter);
    g_engine = &engine;

    if (!engine.begin(millis())) {
        Serial.println("[FATAL] hunt engine could not begin (promiscuous mode failed)");
        return false;
    }
    g_active = true;
    g_lastPhase = engine.phase();
    Serial.printf("[RSSI] probe up channels=%s armed=1 (correlating beacon RSSI with capture outcome)\n",
                  hopText);
    return true;
}

bool rssiProbeActive() { return g_active; }

void rssiProbePump() {
    if (!g_active || g_engine == nullptr) return;

    const uint32_t now = millis();
    g_engine->tick(now);

    const HuntEngine::Phase phase = g_engine->phase();
    if (phase != g_lastPhase) {
        if (phase == HuntEngine::Phase::Capturing) {
            latchAttempt(now);
        } else if (phase == HuntEngine::Phase::Discovering) {
            resolvePendingEmpty(now);  // the last target's window ended; a fresh sweep begins.
        }
        g_lastPhase = phase;
    }

    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        // Liveness + running tally for a tethered smoke test; distinguishes "alive, hunting" from "hung".
        // Surfaces the two degradations a silent untethered run would otherwise hide: a full RSSI table and
        // a flash that has stopped accepting flushes.
        Serial.printf("[RSSI] waiting attempts=%lu captured=%lu%s%s\n",
                      static_cast<unsigned long>(g_attempts), static_cast<unsigned long>(g_captured),
                      g_rssiOverflow.load(std::memory_order_relaxed) ? " (rssi-table-overflow)" : "",
                      g_flushFailures > 0 ? " (flush-failures)" : "");
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary hunts or transmits from a probe (ADR-0015 #8, §4 #4).

namespace sapper {
bool rssiProbeBegin() { return false; }
bool rssiProbeActive() { return false; }
void rssiProbePump() {}
}  // namespace sapper

#endif
