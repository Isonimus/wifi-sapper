/**
 * @file radio_sniffer_esp32.cpp
 * @brief esp_wifi promiscuous implementation of the RadioSniffer seam (ADR-0011). Device-only.
 */
#include "net/radio_sniffer_esp32.h"

#ifndef UNIT_TEST

#include <WiFi.h>
#include <esp_wifi.h>

#include <atomic>

namespace sapper {
namespace {

// The ESP-IDF promiscuous RX callback takes no user pointer, so the active consumer must be reached
// through a file-scope pointer. This is the one unavoidable static (ADR-0011): it holds no protocol
// state, lives only inside this device wrapper, and stop() clears it. It is atomic because the
// callback (Wi-Fi driver task) and begin()/stop() (app task) touch it from different tasks — and on
// the S3 potentially different cores — so a plain pointer would be a data race. Atomicity makes the
// pointer access well-defined; it does NOT hard-join an in-flight callback (see stop()).
std::atomic<FrameConsumer*> g_consumer{nullptr};

/// Runs in the Wi-Fi driver task (ADR-0011, §4 invariant #10): copy the raw frame to the consumer
/// and return. No allocation, no blocking, no parsing here — the pure core does all interpretation.
void onPromiscuousRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    // The MGMT|DATA hardware filter is the primary selector; this guard is a length-validity
    // backstop, not protocol logic (§4 invariant #8 stays intact): the IDF documents
    // WIFI_PKT_MISC's sig_len as not a real frame length, so forwarding one would over-read
    // pkt->payload straight into the core. Drop any packet type that is not a real frame.
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    FrameConsumer* consumer = g_consumer.load(std::memory_order_acquire);
    if (consumer == nullptr || buf == nullptr) return;
    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    // sig_len is the on-air frame length including the trailing FCS; forwarded raw (ADR-0011).
    consumer->onFrame(pkt->payload, static_cast<uint16_t>(pkt->rx_ctrl.sig_len));
}

}  // namespace

bool Esp32RadioSniffer::begin(uint8_t channel, FrameConsumer& consumer) {
    g_consumer.store(&consumer, std::memory_order_release);

    // Promiscuous capture needs the Wi-Fi driver started; STA with no association is the lightest
    // role that leaves the radio free to listen. We never connect — this only sniffs.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // A radio-level filter, not code: deliver only management (beacons) and data (EAPOL) frames, so
    // the callback never even sees control frames (ADR-0011 decision #2, keeping §4 invariant #8).
    const wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };

    // Register the callback and filter, enable promiscuous mode, then pin the channel. On ANY step
    // failing, roll the radio back out of promiscuous mode via stop() before returning false: a
    // later step can fail after esp_wifi_set_promiscuous(true) has already latched the radio on
    // (e.g. a country-restricted channel rejected by set_channel), and the caller reads false as
    // "inactive" and falls through to normal boot — which must not run with the radio still
    // promiscuous. Fail loud AND clean.
    if (esp_wifi_set_promiscuous_rx_cb(&onPromiscuousRx) == ESP_OK &&
        esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
        esp_wifi_set_promiscuous(true) == ESP_OK &&
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
        return true;
    }
    stop();
    return false;
}

bool Esp32RadioSniffer::setChannel(uint8_t channel) {
    // The radio stays promiscuous with the same callback and consumer installed; only the tuned
    // channel changes. Fail loud if the radio rejects it (e.g. a country-restricted channel) so the
    // hop driver can see the sweep is not covering the band it asked for.
    return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK;
}

void Esp32RadioSniffer::stop() {
    // Disable promiscuous mode and detach the consumer. This cannot hard-join a callback already in
    // flight in the driver task — blocking there is forbidden (§4 invariant #10) — so a frame
    // dequeued just before this runs may still complete its onFrame() after stop() returns. The
    // contract in radio_sniffer.h says so; the engine that owns start/stop sequences its read-out of
    // the consumer accordingly (LEDGER, ADR-0011). The atomic store makes the detach well-defined.
    esp_wifi_set_promiscuous(false);
    g_consumer.store(nullptr, std::memory_order_release);
}

}  // namespace sapper

#endif  // UNIT_TEST
