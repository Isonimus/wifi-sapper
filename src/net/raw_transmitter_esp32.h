/**
 * @file raw_transmitter_esp32.h
 * @brief ESP-IDF raw-TX implementation of the RawTransmitter seam (ADR-0027, ADR-0029). Device-only.
 *
 * Wraps esp_wifi_80211_tx behind the pure RawTransmitter interface (raw_transmitter.h). It carries
 * no frame-building logic — it transmits the bytes the pure deauth builder produced (§4 invariant
 * #15). This compiles into every device build (ADR-0029 superseded ADR-0027 decision 3's
 * SAPPER_TEST_HOOKS gate): the raw-TX capability now ships, but a frame is transmitted only when the
 * operator has *armed* deauth through the default-off provisioning toggle — `hunt_loop` injects this
 * transmitter into the HuntEngine only when armed, and a disarmed device transmits nothing
 * (§4 invariant #16). It is excluded only from the native lane (no esp_wifi there).
 */
#pragma once

#if !defined(UNIT_TEST)

#include "net/raw_transmitter.h"

namespace sapper {

/// esp_wifi_80211_tx raw transmitter. transmit() sends on the STA interface, which the sniffer has
/// already brought up promiscuous on the target channel. The SDK sanity-check bypass (the
/// pioarduino/Bruce weak-symbol override; ADR-0027) lives in the .cpp. There is deliberately no
/// begin()/bypassActive() self-check: it could only call the override this same TU defines, which
/// always succeeds, so it would prove nothing about whether esp_wifi_80211_tx is actually wired to
/// the override. The real signal of an ineffective bypass is transmit() returning false at runtime.
class Esp32RawTransmitter : public RawTransmitter {
public:
    bool transmit(const uint8_t* frame, uint16_t len) override;
};

}  // namespace sapper

#endif  // !UNIT_TEST
