/**
 * @file rf_discover_probe.cpp
 * @brief Implementation of the slice-0014 bench hop-and-discover probe (ADR-0013). Device-only.
 */
#include "rf_discover_probe.h"

#if defined(SAPPER_TEST_HOOKS)

#include <Arduino.h>

#include <cstdlib>
#include <cstring>

#include "net/ap_registry.h"
#include "net/channel_hopper.h"
#include "net/radio_sniffer_esp32.h"

namespace sapper {
namespace {

constexpr uint32_t kDefaultDwellMs = 300;  // ≈3× the 102.4ms beacon interval (ADR-0013 decision #5).
constexpr uint32_t kHeartbeatIntervalMs = 2000;
constexpr uint8_t kMinChannel = 1;
constexpr uint8_t kMaxChannel = 14;

bool g_active = false;
size_t g_reported = 0;
bool g_overflowReported = false;
uint32_t g_lastHeartbeatMs = 0;
ChannelHopper* g_hopper = nullptr;
ApRegistry* g_registry = nullptr;
Esp32RadioSniffer* g_sniffer = nullptr;

/// Parse a comma-separated channel list ("1,6,11") into @p out; false on any malformed or
/// out-of-range token, an empty list, or more than the sweep can hold. Fails loud rather than
/// silently sweeping a partial or wrong band.
bool parseChannelList(const char* text, uint8_t out[kMaxHopChannels], size_t& count) {
    count = 0;
    const char* cursor = text;
    while (*cursor != '\0') {
        char* end = nullptr;
        const long channel = std::strtol(cursor, &end, 10);
        if (end == cursor) return false;  // not a number where one was expected.
        if (channel < kMinChannel || channel > kMaxChannel) return false;
        if (count >= kMaxHopChannels) return false;  // more channels than a sweep can hold.
        out[count++] = static_cast<uint8_t>(channel);
        cursor = end;
        if (*cursor == ',') {
            ++cursor;
            if (*cursor == '\0') return false;  // trailing comma: a malformed list, not a channel.
        } else if (*cursor != '\0') {
            return false;  // a separator other than a comma is malformed.
        }
    }
    return count > 0;
}

}  // namespace

bool rfDiscoverProbeBegin() {
    const char* hopText = SAPPER_TEST_RF_HOP;
    if (hopText == nullptr || hopText[0] == '\0') return false;  // inactive: normal boot runs.

    uint8_t channels[kMaxHopChannels] = {0};
    size_t count = 0;
    if (!parseChannelList(hopText, channels, count)) {
        Serial.printf("[FATAL] SAPPER_TEST_RF_HOP not a channel list in %u-%u: '%s'\n", kMinChannel,
                      kMaxChannel, hopText);
        return false;
    }

    // Dwell is optional: empty falls back to the spec-grounded default; a non-empty but malformed
    // value fails loud rather than silently sweeping too fast to catch a beacon (ADR-0013).
    uint32_t dwellMs = kDefaultDwellMs;
    const char* dwellText = SAPPER_TEST_RF_DWELL_MS;
    if (dwellText != nullptr && dwellText[0] != '\0') {
        char* end = nullptr;
        const long parsed = std::strtol(dwellText, &end, 10);
        if (end == dwellText || *end != '\0' || parsed <= 0) {
            Serial.printf("[FATAL] SAPPER_TEST_RF_DWELL_MS not a positive integer: '%s'\n", dwellText);
            return false;
        }
        dwellMs = static_cast<uint32_t>(parsed);
    }

    // File-scope lifetime: the RX callback forwards into the registry for the probe's whole run, and
    // pump() drives the hopper each loop. This is the capture path a discovery run really wires
    // (ADR-0013): Esp32RadioSniffer → ApRegistry, retuned by ChannelHopper.
    static ApRegistry registry;
    static ChannelHopper hopper(channels, count, dwellMs);
    static Esp32RadioSniffer sniffer;
    g_registry = &registry;
    g_hopper = &hopper;
    g_sniffer = &sniffer;

    registry.setCurrentChannel(hopper.currentChannel());
    if (!sniffer.begin(hopper.currentChannel(), registry)) {
        Serial.println("[FATAL] promiscuous begin failed");
        return false;
    }
    hopper.reset(millis());
    g_active = true;

    Serial.printf("[HOP] sweeping channels=%s dwell=%lums count=%u\n", hopText,
                  static_cast<unsigned long>(dwellMs), static_cast<unsigned>(count));
    Serial.printf("[HOP] channel=%u\n", hopper.currentChannel());
    return true;
}

bool rfDiscoverProbeActive() { return g_active; }

void rfDiscoverProbePump() {
    if (!g_active || g_hopper == nullptr || g_registry == nullptr || g_sniffer == nullptr) return;

    if (g_hopper->tick(millis())) {
        const uint8_t channel = g_hopper->currentChannel();
        if (!g_sniffer->setChannel(channel)) {
            // esp_wifi_set_channel leaves the radio on its previous channel on failure, so the
            // registry's fallback must NOT advance either — otherwise a DS-Param-less beacon still
            // arriving on the old channel would be tagged with one the radio never reached, the
            // guessed channel ADR-0013 decision #3 forbids. Leave the fallback where it was.
            Serial.printf("[ERROR] setChannel(%u) failed — channel restricted?\n", channel);
        } else {
            g_registry->setCurrentChannel(channel);  // radio confirmed retuned: advance the fallback.
            Serial.printf("[HOP] channel=%u\n", channel);
        }
    }

    // Report every AP discovered since the last pump. count() publishes the append-only table, so
    // entries [g_reported, count) are fully written and safe to read (ADR-0013).
    const size_t discovered = g_registry->count();
    for (; g_reported < discovered; ++g_reported) {
        const DiscoveredAp& ap = g_registry->at(g_reported);
        Serial.printf("[DISCOVER] ap bssid=%02x:%02x:%02x:%02x:%02x:%02x ssid=%s channel=%u\n",
                      ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5],
                      ap.ssid, ap.channel);
    }
    if (g_registry->overflowed() && !g_overflowReported) {
        g_overflowReported = true;
        // Not fatal, but the enumeration is incomplete — surface it rather than hide the drop.
        Serial.printf("[ERROR] AP registry overflowed at %u entries; discovery incomplete\n",
                      static_cast<unsigned>(kMaxDiscoveredAps));
    }

    const uint32_t now = millis();
    if (now - g_lastHeartbeatMs >= kHeartbeatIntervalMs) {
        g_lastHeartbeatMs = now;
        // Liveness for the artifact: distinguishes "device alive, sweeping" from "device hung".
        Serial.printf("[HOP] waiting elapsed=%lums discovered=%u\n", static_cast<unsigned long>(now),
                      static_cast<unsigned>(discovered));
    }
}

}  // namespace sapper

#else  // no SAPPER_TEST_HOOKS: no shipped binary hops or discovers (ADR-0013 decision #7).

namespace sapper {
bool rfDiscoverProbeBegin() { return false; }
bool rfDiscoverProbeActive() { return false; }
void rfDiscoverProbePump() {}
}  // namespace sapper

#endif
