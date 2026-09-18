/**
 * @file radio_sniffer_esp32.h
 * @brief ESP-IDF promiscuous-mode implementation of the RadioSniffer seam (ADR-0011). Device-only.
 *
 * Wraps esp_wifi promiscuous capture behind the pure RadioSniffer interface (radio_sniffer.h). It
 * carries no protocol logic — the RX callback copies each raw frame to the registered consumer and
 * returns (ADR-0011, §4 invariant #10). Device-only (esp_wifi); the pure seam and the capture core
 * it feeds are host-tested on the native lane, and the on-air behaviour is proved by the slice-0012
 * verify script.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/radio_sniffer.h"

namespace sapper {

/// esp_wifi promiscuous sniffer. begin() sets the channel, installs the RX callback, and enters
/// promiscuous mode filtered — at the radio — to management + data frames, so beacons and EAPOL
/// arrive while control frames do not (a hardware filter, not code-level protocol logic; ADR-0011).
class Esp32RadioSniffer : public RadioSniffer {
public:
    bool begin(uint8_t channel, FrameConsumer& consumer) override;
    bool setChannel(uint8_t channel) override;
    void stop() override;
};

}  // namespace sapper

#endif  // UNIT_TEST
