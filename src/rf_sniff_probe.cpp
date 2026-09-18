/**
 * @file rf_sniff_probe.cpp
 * @brief Implementation of the slice-0012 bench sniffer probe (ADR-0011). Device-only.
 */
#include "rf_sniff_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include <cstdio>
#include <cstdlib>

#include "net/handshake_collector.h"
#include "net/handshake_consumer.h"
#include "net/radio_sniffer_esp32.h"

namespace sapper {
namespace {

constexpr uint32_t kHeartbeatIntervalMs = 2000;

bool g_active = false;
bool g_beaconReported = false;
uint32_t g_lastHeartbeatMs = 0;
uint8_t g_bssid[6] = {0};
HandshakeCollector* g_collector = nullptr;

/// Parse "aa:bb:cc:dd:ee:ff" (case-insensitive) into six octets; false on any malformed field.
bool parseBssid(const char* text, uint8_t out[6]) {
    unsigned v[6];
    int consumed = 0;
    // %n records how many characters matched (it is not itself an assignment, so a full match still
    // returns 6). Requiring the whole string be consumed rejects trailing garbage
    // ("aa:bb:cc:dd:ee:ff:gg"), honouring this function's "false on any malformed field" contract.
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

}  // namespace

bool rfSniffProbeBegin() {
    const char* bssidText = SAPPER_TEST_RF_BSSID;
    if (bssidText == nullptr || bssidText[0] == '\0') return false;  // inactive: normal boot runs.

    if (!parseBssid(bssidText, g_bssid)) {
        Serial.printf("[FATAL] SAPPER_TEST_RF_BSSID malformed: %s\n", bssidText);
        return false;
    }
    char* end = nullptr;
    const long channel = std::strtol(SAPPER_TEST_RF_CHANNEL, &end, 10);
    // Reject empty or trailing-garbage values ("6ghz" -> 6) rather than silently truncating them, so
    // a typo'd env var fails loud instead of quietly sniffing the wrong channel.
    if (end == SAPPER_TEST_RF_CHANNEL || *end != '\0' || channel < 1 || channel > 14) {
        Serial.printf("[FATAL] SAPPER_TEST_RF_CHANNEL not an integer in 1-14: '%s'\n",
                      SAPPER_TEST_RF_CHANNEL);
        return false;
    }

    // The capture path the device really wires (ADR-0011): sniffer -> HandshakeConsumer -> collector.
    // File-scope lifetime because the RX callback forwards into them for the probe's whole run.
    static HandshakeCollector collector(g_bssid, static_cast<uint8_t>(channel));
    static HandshakeConsumer consumer(collector);
    static Esp32RadioSniffer sniffer;
    g_collector = &collector;

    if (!sniffer.begin(static_cast<uint8_t>(channel), consumer)) {
        Serial.println("[FATAL] promiscuous begin failed");
        return false;
    }
    g_active = true;
    Serial.printf("[SNIFF] listening channel=%ld bssid=%s\n", channel, bssidText);
    return true;
}

bool rfSniffProbeActive() { return g_active; }

void rfSniffProbePump() {
    if (!g_active || g_collector == nullptr) return;

    if (!g_beaconReported && g_collector->hasBeacon()) {
        g_beaconReported = true;
        // The pass line: the target AP's beacon reached the pure core through the real consumer.
        Serial.printf("[SNIFF] beacon bssid=%s ssid=%s\n", SAPPER_TEST_RF_BSSID,
                      g_collector->handshake().ssid);
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        // Liveness for the artifact: distinguishes "device alive, AP not seen" from "device hung".
        Serial.printf("[SNIFF] waiting elapsed=%lums beacon=%d\n", static_cast<unsigned long>(now),
                      g_beaconReported ? 1 : 0);
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary sniffs (ADR-0011 decision #5).

namespace sapper {
bool rfSniffProbeBegin() { return false; }
bool rfSniffProbeActive() { return false; }
void rfSniffProbePump() {}
}  // namespace sapper

#endif
