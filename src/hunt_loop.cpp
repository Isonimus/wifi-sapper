/**
 * @file hunt_loop.cpp
 * @brief The shipped hunt→enqueue→drain→upload loop (ADR-0017 decision #8). Device-only.
 */
#include "hunt_loop.h"

#ifndef UNIT_TEST

#include <Arduino.h>

#include <cstring>
#include <new>

#include "net/ap_registry.h"
#include "net/capture_queue.h"
#include "net/capture_store_littlefs.h"
#include "net/hunt_engine.h"
#include "net/radio_sniffer_esp32.h"
#include "net/station_control_esp32.h"
#include "net/uploader_wpasec.h"

#ifdef SAPPER_TEST_HOOKS
// The operator's real handshake pcap, embedded by embed_test_pcap.py (empty when none is given). At
// global scope so its own `namespace sapper` lands correctly; included only in test-hooks builds.
#include "gen/wpasec_test_pcap.h"
#endif

namespace sapper {
namespace {

// The shipped sweep: the three non-overlapping 2.4 GHz channels most APs sit on, so a cycle is quick
// and covers the common case. A fuller 1-13 sweep is a later tuning decision (LEDGER), not a floor.
uint8_t g_hopChannels[3] = {1, 6, 11};

// The spine, file-scope so it lives for the device's whole run. Order matters: the relay is built
// before the engine (which observes it) and the supervisor (which the relay targets), resolving the
// construction cycle CaptureReadyRelay exists for.
Esp32RadioSniffer g_sniffer;
ApRegistry g_registry;
CaptureReadyRelay g_relay;
HuntEngine g_engine(g_sniffer, g_registry, g_relay, g_hopChannels, 3, HuntConfig{});
LittleFsCaptureStore g_store;
CaptureQueue g_queue(g_store);
WpaSecUploader g_uploader;

// Our own copy of the credentials, at file scope so the station adapter's ssid/pass pointers and the
// supervisor's key pointer stay valid for the device's whole run. Copying here (rather than holding
// the caller's ProvisioningRecord) means a caller may pass a stack temporary safely — the earlier
// design, which held the caller's pointers, dangled when the verify probe passed a local record.
ProvisioningRecord g_creds;

// Constructed in huntLoopBegin() once credentials are known: the station adapter needs the SSID/pass,
// the supervisor the wpa-sec key. Placement-new into aligned storage keeps them at file scope without
// a heap allocation or a default-constructible requirement on types that hold references.
alignas(Esp32StationControl) uint8_t g_stationStorage[sizeof(Esp32StationControl)];
alignas(UploadSupervisor) uint8_t g_supervisorStorage[sizeof(UploadSupervisor)];
Esp32StationControl* g_station = nullptr;
UploadSupervisor* g_supervisor = nullptr;
bool g_running = false;

#ifdef SAPPER_TEST_HOOKS
// A synthetic wpa-sec-valid handshake (beacon + M1 + M2) for the on-air verify's stimulus, built with
// the same minimal frames the native tests use so serializeHandshake emits a well-formed pcap. It is
// not a crackable capture, so the live service classifies it — the verify proves the TLS/POST/parse
// path, not a crack (slice-0018 Scenario G).
const uint8_t kStimulusBssid[6] = {0x02, 0x53, 0x41, 0x50, 0x50, 0x52};
const uint8_t kStimulusClient[6] = {0x06, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};

// pcap on-the-wire is little-endian (kPcapMagicMicroseconds). Read the fields we need directly.
uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void appendEapol(uint8_t* frame, uint16_t& len, const uint8_t bssid[6], const uint8_t client[6],
                 uint16_t keyInfo, bool fromAp) {
    uint8_t* p = frame;
    std::memset(p, 0, 24);
    p[0] = 0x08;                    // data frame.
    p[1] = fromAp ? 0x02 : 0x01;   // From-DS / To-DS.
    if (fromAp) {
        std::memcpy(&p[4], client, 6);
        std::memcpy(&p[10], bssid, 6);
    } else {
        std::memcpy(&p[4], bssid, 6);
        std::memcpy(&p[10], client, 6);
    }
    std::memcpy(&p[16], bssid, 6);
    const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    std::memcpy(&p[24], snap, 8);
    uint8_t* eapol = &p[32];
    std::memset(eapol, 0, 99);
    eapol[0] = 0x02;                                  // EAPOL version.
    eapol[1] = 0x03;                                  // EAPOL-Key.
    eapol[3] = 95;                                    // body length.
    eapol[4] = 0x02;                                  // WPA2 key descriptor.
    eapol[5] = static_cast<uint8_t>(keyInfo >> 8);
    eapol[6] = static_cast<uint8_t>(keyInfo & 0xFF);
    len = 32 + 99;
}
#endif  // SAPPER_TEST_HOOKS

}  // namespace

bool huntLoopBegin(const ProvisioningRecord& creds, const UploadSupervisorConfig& config) {
    if (!g_store.begin()) {
        Serial.println("[FATAL] capture store (LittleFS) mount failed");
        return false;
    }
    g_creds = creds;  // own the credentials so the adapter/supervisor pointers never dangle.
    g_station = new (g_stationStorage) Esp32StationControl(g_creds.ssid, g_creds.pass);
    g_supervisor = new (g_supervisorStorage)
        UploadSupervisor(g_engine, g_queue, g_uploader, *g_station, g_creds.key, config);
    g_relay.setTarget(*g_supervisor);

    if (!g_engine.begin(millis())) {
        Serial.println("[FATAL] hunt engine could not begin (promiscuous mode failed)");
        return false;
    }
    g_supervisor->begin(millis());
    g_running = true;
    Serial.println("[HUNT] discovering channels=1,6,11 (shipped hunt+upload loop)");
    return true;
}

void huntLoopPump() {
    if (!g_running) return;
    const uint32_t now = millis();
    g_engine.tick(now);
    g_supervisor->tick(now);
}

const DrainOutcome& huntLoopLastDrain() {
    static DrainOutcome none;
    return g_supervisor != nullptr ? g_supervisor->lastDrain() : none;
}

uint32_t huntLoopDrainCount() { return g_supervisor != nullptr ? g_supervisor->drainCount() : 0; }

#ifdef SAPPER_TEST_HOOKS
namespace {

// Inject the operator's embedded real handshake (gen/wpasec_test_pcap.h). The pcap may be link type
// 105 (raw 802.11) or 127 (radiotap) — the Adversary's captures are radiotap, so strip the per-frame
// radiotap header before feeding raw 802.11 to the collector, which keeps beacon + M1..M4 for the
// target BSSID. Re-serialized and uploaded, a real handshake is what exercises accepted/duplicate.
void injectRealPcap() {
    const uint8_t* pcap = kWpaSecTestPcap;
    const size_t len = kWpaSecTestPcapLen;
    if (len < 24) {
        Serial.println("[ERROR] embedded test pcap too small");
        return;
    }
    const bool radiotap = readLe32(pcap + 20) == 127;  // network field: 127 = radiotap-prefixed.

    // Resolve one record's 802.11 body (radiotap stripped). Returns false on a truncated/short record.
    auto frameOf = [&](size_t recOff, const uint8_t*& frame, uint16_t& frameLen) -> bool {
        if (recOff + 16 > len) return false;
        const uint32_t inclLen = readLe32(pcap + recOff + 8);
        const uint8_t* pkt = pcap + recOff + 16;
        if (recOff + 16 + inclLen > len) return false;
        if (radiotap) {
            if (inclLen < 4) return false;
            const uint16_t itLen = readLe16(pkt + 2);  // radiotap it_len.
            if (itLen >= inclLen) return false;
            frame = pkt + itLen;
            frameLen = static_cast<uint16_t>(inclLen - itLen);
        } else {
            frame = pkt;
            frameLen = static_cast<uint16_t>(inclLen);
        }
        return true;
    };

    const uint8_t* first = nullptr;
    uint16_t firstLen = 0;
    if (!frameOf(24, first, firstLen) || firstLen < 24) {
        Serial.println("[ERROR] embedded test pcap has no usable first frame");
        return;
    }
    uint8_t bssid[6];
    std::memcpy(bssid, first + 16, 6);  // Addr3 = BSSID; the collector keeps only this AP's frames.
    HandshakeCollector collector(bssid, 0);

    for (size_t off = 24; off + 16 <= len;) {
        const uint32_t inclLen = readLe32(pcap + off + 8);
        // Guard the advance against a corrupt/oversized length: without this, off + 16 + inclLen can wrap
        // size_t, regress `off`, and spin the loop forever on a malformed embedded pcap (verify tooling).
        if (inclLen > len - off - 16) break;
        const uint8_t* frame = nullptr;
        uint16_t frameLen = 0;
        if (frameOf(off, frame, frameLen) && frameLen >= 24) collector.ingest(frame, frameLen);
        off += 16 + inclLen;
    }

    if (!collector.isWpaSecValid()) {
        Serial.println("[ERROR] embedded test pcap did not yield a wpa-sec-valid handshake");
        return;
    }
    const CapturedHandshake& hs = collector.handshake();
    Serial.printf("[UPLOAD] real handshake reconstructed: ssid='%s' beacon=%d M1=%d M2=%d M3=%d M4=%d\n",
                  hs.ssid, hs.hasBeacon(), hs.has(HandshakeMessage::M1), hs.has(HandshakeMessage::M2),
                  hs.has(HandshakeMessage::M3), hs.has(HandshakeMessage::M4));
    Serial.println("[UPLOAD] injecting REAL handshake (embedded pcap) through the capture-ready seam");
    g_relay.onCaptureReady(hs);
}

}  // namespace

void huntLoopInjectStimulus() {
    if (!g_running) return;
    if (kWpaSecTestPcapLen > 0) {  // an operator pcap is embedded — use it instead of the synthetic.
        injectRealPcap();
        return;
    }
    HandshakeCollector collector(kStimulusBssid, /*channel=*/1);  // matches the stimulus beacon below.

    uint8_t beacon[64];
    std::memset(beacon, 0, sizeof(beacon));
    beacon[0] = 0x80;                       // beacon.
    std::memset(&beacon[4], 0xFF, 6);       // broadcast.
    std::memcpy(&beacon[10], kStimulusBssid, 6);
    std::memcpy(&beacon[16], kStimulusBssid, 6);
    uint16_t bp = 24 + 12;                  // fixed body (timestamp/interval/capability).
    beacon[bp++] = 0x00;                    // SSID element,
    beacon[bp++] = 0x06;                    // length 6,
    std::memcpy(&beacon[bp], "SAPPER", 6);
    bp += 6;
    beacon[bp++] = 0x03;                    // DS Parameter Set,
    beacon[bp++] = 0x01;
    beacon[bp++] = 0x01;                    // channel 1.
    collector.ingest(beacon, bp);

    uint8_t frame[160];
    uint16_t len = 0;
    appendEapol(frame, len, kStimulusBssid, kStimulusClient, 0x0088, /*fromAp=*/true);   // M1 (Ack).
    collector.ingest(frame, len);
    appendEapol(frame, len, kStimulusBssid, kStimulusClient, 0x0108, /*fromAp=*/false);  // M2 (MIC).
    collector.ingest(frame, len);

    if (!collector.isWpaSecValid()) {
        Serial.println("[ERROR] stimulus handshake is not wpa-sec-valid — verify build is broken");
        return;
    }
    Serial.println("[UPLOAD] injecting stimulus handshake through the capture-ready seam");
    g_relay.onCaptureReady(collector.handshake());  // the same seam the engine calls (§4 invariant #2).
}
#endif  // SAPPER_TEST_HOOKS

}  // namespace sapper

#endif  // UNIT_TEST
