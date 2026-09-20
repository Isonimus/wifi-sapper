/**
 * @file raw_transmitter_esp32.h
 * @brief ESP-IDF raw-TX implementation of the RawTransmitter seam (ADR-0027). Device-only AND
 *        SAPPER_TEST_HOOKS-gated — the raw-TX capability is absent from any shipped binary.
 *
 * Wraps esp_wifi_80211_tx behind the pure RawTransmitter interface (raw_transmitter.h). It carries
 * no frame-building logic — it transmits the bytes the pure deauth builder produced (§4 invariant
 * #15). The whole class, and the SDK sanity-check bypass it depends on, compile only under
 * SAPPER_TEST_HOOKS (ADR-0027 decision 3): a non-hooks build cannot transmit a deauth frame at all.
 * Until the allowlist-gated unattended-deauth operating mode ships (LEDGER), this gate stands.
 */
#pragma once

#if !defined(UNIT_TEST) && defined(SAPPER_TEST_HOOKS)

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

#endif  // !UNIT_TEST && SAPPER_TEST_HOOKS
