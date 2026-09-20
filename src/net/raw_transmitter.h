/**
 * @file raw_transmitter.h
 * @brief The raw-bytes->radio TX seam: put one fully-formed 802.11 frame on the air (ADR-0027).
 *
 * The hardware-facing half of the deauth actuation path, and the transmit mirror of RadioSniffer's
 * RX seam (radio_sniffer.h). A RawTransmitter takes the bytes the pure builder (deauth.h) produced
 * and transmits them; it carries no frame-building or protocol logic — raw bytes only (§4 invariant
 * #15). This header is pure so the abstract seam compiles on the native lane and a host test drives
 * the builder into a fake transmitter through the same transmit() the device path uses (§4 invariant
 * #2). The ESP-IDF implementation is device-only (raw_transmitter_esp32.h) and ships in every device
 * build; transmitting is gated at runtime by the operator's default-off deauth arm toggle, not at
 * compile time (ADR-0029 superseded ADR-0027 #3's SAPPER_TEST_HOOKS gate; §4 invariant #16).
 */
#pragma once

#include <cstdint>

namespace sapper {

/// Transmits fully-formed 802.11 frames. Unlike the RX seam, the underlying esp_wifi_80211_tx is a
/// direct call with no context-free callback, so this needs none of ADR-0011's file-scope machinery.
class RawTransmitter {
public:
    virtual ~RawTransmitter() = default;
    /// Put @p len bytes of a complete 802.11 frame on the air. Returns false if the radio rejected
    /// the frame (e.g. the SDK sanity-check bypass is not active, or the interface is not up).
    virtual bool transmit(const uint8_t* frame, uint16_t len) = 0;
};

}  // namespace sapper
