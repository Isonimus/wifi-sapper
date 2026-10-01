/**
 * @file rssi_probe.cpp
 * @brief RSSI↔capture-correlation probe (ADR-0067). Device-only, SAPPER_TEST_HOOKS-gated.
 *
 * Runs the real, unmodified HuntEngine in discovery order (armed, so captures actually fire) while a
 * throwaway RssiTapSniffer records, for the target currently under capture, the strongest beacon
 * rx_ctrl.rssi heard while the engine is parked on that target's channel (a per-attempt, on-channel value —
 * see the tap block below for why a global discovery-time table was wrong). The production
 * RadioSniffer seam is not touched — §4 #8 (raw bytes only) holds; the RSSI never reaches the engine. The
 * engine wires NO cracked/submitted gate (no setCrackedLookup/setSubmittedLookup) and never uploads, so
 * it targets and attempts EVERY encrypted AP regardless of wpa-sec state — a lab whose handshakes are all
 * already uploaded needs no memory wipe, and "captured" is a pure on-air capture event.
 *
 * The multi-hour run is untethered (no serial visible), so each resolved capture attempt is accumulated
 * into a compact per-band histogram (net/rssi_histogram.h) persisted to LittleFS. The histogram is dumped
 * to serial both at boot and on a periodic cadence (kDumpIntervalMs) — periodic because this board's native
 * USB re-enumerates on reset, so a reader cannot reliably catch a boot-only dump, and a reader attached to a
 * running board never triggers a reboot; a periodic dump is caught by simply attaching. A run tag
 * (SAPPER_TEST_RSSI_RUN) starts the counters fresh when it differs from the persisted file and resumes them
 * when it matches. Both the file and the dump are identity-free (per-band counts only), so §4 #25 holds by
 * construction. A per-attempt `[RSSI] rssi=…` row is still printed for the tethered smoke test.
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
/// Full-histogram dump cadence. The untethered readout must be catchable without a reboot (this board's
/// native USB re-enumerates on reset, so a boot-only dump is unreliable to catch), so the authoritative
/// persisted totals are re-emitted periodically. Kept under the reader's dump timeout (scripts/0068) so an
/// attach always catches a block within one interval.
constexpr uint32_t kDumpIntervalMs = 15000;
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
//
// A target is binned by the STRONGEST beacon RSSI heard for it while the engine is parked on its channel
// capturing it — a per-attempt, on-channel value, not a global discovery-time table. This is deliberate
// (ADR-0067, 4th amendment): the first design kept a global per-BSSID table and a multi-hour field run
// exposed two fatal defects. (1) It was a fixed 64-entry (kMaxDiscoveredAps) append-only table that never
// reset, but the engine re-discovers past 64 APs (HuntEngine resets ApRegistry each enumeration cycle), so
// once full it went blind and every later target read back kRssiUnknown — 89% of that run. (2) Its
// latest-wins value was corrupted by adjacent-channel bleed: a strong on-channel reading overwritten by a
// faint cross-channel one while the radio was parked elsewhere, skewing every recorded RSSI toward the noise
// floor. Reading only the active target, only during its on-channel capture window, and keeping the max
// removes both by construction — no unbounded table to overflow, and bleed (always weaker than the
// on-channel signal) can never win the max.

FrameConsumer* g_tapConsumer = nullptr;  // set on the app task before the callback can fire; read-only after.

/// The target whose capture window is open, as its 6-byte BSSID packed little-endian into the low 48 bits of
/// one word (0 = no window open — an all-zero BSSID is never a real AP). Packing into a single atomic lets the
/// driver task read the target as one indivisible load, so it can never see a half-written BSSID during the
/// app task's window handoff (a per-byte shared buffer would be a torn read — a data race — at every boundary).
std::atomic<uint64_t> g_attemptTarget{0};
std::atomic<int8_t> g_attemptTapRssiMax{kRssiUnknown};  // strongest on-channel beacon RSSI this window (kRssiUnknown = none yet).

bool bssidEq(const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, 6) == 0; }

/// Pack a 6-byte BSSID little-endian into the low 48 bits of a word for the single-atomic target handoff.
uint64_t packBssid(const uint8_t* bssid) {
    uint64_t v = 0;
    for (size_t i = 0; i < 6; ++i) v |= static_cast<uint64_t>(bssid[i]) << (8 * i);
    return v;
}

/// The ESP-IDF promiscuous RX callback: tap rx_ctrl.rssi for the target under capture, then forward the raw
/// frame bytes to the engine exactly as Esp32RadioSniffer does (no protocol logic added to the seam; the
/// RSSI tap is probe-only and never reaches the consumer). Runs in the Wi-Fi driver task — bounded, no
/// allocation, no blocking (§4 #10).
void onTapRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;  // MISC sig_len is not a frame length.
    if (g_tapConsumer == nullptr || buf == nullptr) return;
    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    const uint16_t len = static_cast<uint16_t>(pkt->rx_ctrl.sig_len);
    // Tap only the target whose capture window is open, only from its own beacons. During that window the
    // radio is parked on the target's channel, so its beacons arrive on-channel (strong) while other APs
    // arrive only as weaker bleed — matching the active BSSID and keeping the max yields the clean on-channel
    // signal. The acquire pairs with latchAttempt's release, so the cleared max is seen before any update.
    if (type == WIFI_PKT_MGMT && len >= kMgmtHeaderLen && pkt->payload[0] == kBeaconFrameControl0) {
        const uint64_t target = g_attemptTarget.load(std::memory_order_acquire);
        if (target != 0 && target == packBssid(pkt->payload + kBssidOffset)) {
            const int8_t rssi = static_cast<int8_t>(pkt->rx_ctrl.rssi);
            const int8_t cur = g_attemptTapRssiMax.load(std::memory_order_relaxed);
            // kRssiUnknown (0) is out of the real (negative dBm) range, so it must be treated as "no reading
            // yet" rather than a maximum — otherwise the sentinel would never be beaten by a real value.
            if (cur == kRssiUnknown || rssi > cur) g_attemptTapRssiMax.store(rssi, std::memory_order_relaxed);
        }
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

/// Latches the current capture attempt so the pump can pair it with an outcome. The RSSI is not held here:
/// it is accumulated on the driver task into g_attemptTapRssiMax during the window and read at resolve.
struct Attempt {
    bool active = false;
    uint8_t bssid[6] = {0};
    uint32_t startMs = 0;
    uint8_t channel = 0;
};

Attempt g_attempt;
uint32_t g_attempts = 0;
uint32_t g_captured = 0;
uint32_t g_flushFailures = 0;  // persisted-histogram flush failures, surfaced in the heartbeat (fail loud).

/// Record one resolved attempt: accumulate it into the persisted histogram, flush, and (for the tethered
/// smoke test) print an identity-free row.
void resolveAttempt(const Attempt& a, uint32_t nowMs, bool captured) {
    // Close the on-channel RSSI window (clear the target so the tap stops matching) and read its strongest
    // reading. kRssiUnknown here means the target's beacon was never heard while we were parked on it — a
    // legitimate na (a target too weak to beacon-hear during its own capture window), not the table-overflow
    // blindness the old global table produced.
    g_attemptTarget.store(0, std::memory_order_release);
    const int8_t rssi = g_attemptTapRssiMax.load(std::memory_order_acquire);
    ++g_attempts;
    if (captured) ++g_captured;
    recordAttempt(g_hist, rssi, captured);
    // Flush every attempt: the run is untethered and may lose power, so at most the current attempt is
    // ever at risk. The file is tiny (kRssiHistogramBytes) and written far less than once a second. A
    // failure keeps the RAM counts intact (the next flush may land) but is counted and surfaced in the
    // heartbeat — a silently non-persisting run would look empty on reattach with no clue why.
    if (!saveHistogram(g_hist)) ++g_flushFailures;
    if (rssi == kRssiUnknown) {
        Serial.printf("[RSSI] rssi=na channel=%u ms=%lu outcome=%s\n", a.channel,
                      static_cast<unsigned long>(nowMs - a.startMs), captured ? "captured" : "empty");
    } else {
        Serial.printf("[RSSI] rssi=%d channel=%u ms=%lu outcome=%s\n", rssi, a.channel,
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
uint32_t g_lastDumpMs = 0;

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
    // Open the on-channel RSSI window: clear the running max, then publish the target BSSID as one atomic
    // word. The release on g_attemptTarget pairs with the acquire in onTapRx (the driver sees the cleared max
    // before it can match this target), and packing into one atomic means the driver never reads a
    // half-written BSSID during the handoff.
    g_attemptTapRssiMax.store(kRssiUnknown, std::memory_order_relaxed);
    g_attemptTarget.store(packBssid(bssid), std::memory_order_release);
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
        // Liveness for a tethered smoke test; distinguishes "alive, hunting" from "hung". These counters are
        // THIS BOOT only (g_attempts/g_captured are RAM, reset at every boot) — the `session_` prefix is
        // deliberate so they are never mistaken for the persisted totals, which live in the periodic
        // `[RSSI] histogram` block below (a boot-counter read past a reboot is what faked a resume bug).
        // Surfaces the degradation a silent untethered run would otherwise hide: a flash that has stopped
        // accepting flushes (the old per-BSSID table and its overflow flag are gone — the RSSI is now a
        // per-attempt on-channel window with nothing to overflow).
        Serial.printf("[RSSI] waiting session_attempts=%lu session_captured=%lu%s\n",
                      static_cast<unsigned long>(g_attempts), static_cast<unsigned long>(g_captured),
                      g_flushFailures > 0 ? " (flush-failures)" : "");
    }

    if (now - g_lastDumpMs >= kDumpIntervalMs) {
        g_lastDumpMs = now;
        dumpHistogram(g_hist);  // authoritative persisted totals; catchable by attaching, no reboot needed.
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
