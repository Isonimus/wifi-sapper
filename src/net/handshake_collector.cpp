/**
 * @file handshake_collector.cpp
 * @brief Implementation of the pure handshake accumulator (ADR-0009).
 */
#include "net/handshake_collector.h"

#include <cstring>

namespace sapper {
namespace {

/// Map a handshake message to its slot in CapturedHandshake::msg (M1..M4 → 0..3), or -1 if the
/// message is not one of the four pairwise messages.
int messageSlot(HandshakeMessage message) {
    switch (message) {
        case HandshakeMessage::M1: return 0;
        case HandshakeMessage::M2: return 1;
        case HandshakeMessage::M3: return 2;
        case HandshakeMessage::M4: return 3;
        case HandshakeMessage::Unknown: return -1;
    }
    return -1;
}

void store(CapturedFrame& slot, const uint8_t* frame, uint16_t len) {
    std::memcpy(slot.data, frame, len);
    slot.len = len;
}

}  // namespace

bool CapturedHandshake::has(HandshakeMessage message) const {
    const int slot = messageSlot(message);
    return slot >= 0 && msg[slot].present();
}

bool CapturedHandshake::isWpaSecValid() const {
    return hasBeacon() && has(HandshakeMessage::M1) && has(HandshakeMessage::M2);
}

bool CapturedHandshake::isComplete() const {
    return has(HandshakeMessage::M1) && has(HandshakeMessage::M2) && has(HandshakeMessage::M3) &&
           has(HandshakeMessage::M4);
}

HandshakeCollector::HandshakeCollector(const uint8_t targetBssid[6], uint8_t channel) {
    std::memcpy(handshake_.bssid, targetBssid, sizeof(handshake_.bssid));
    handshake_.channel = channel;
}

bool HandshakeCollector::matchesTarget(const uint8_t* frame, uint16_t len) const {
    const uint8_t* bssid = frameBssid(frame, len);
    return bssid != nullptr && std::memcmp(bssid, handshake_.bssid, sizeof(handshake_.bssid)) == 0;
}

void HandshakeCollector::ingest(const uint8_t* frame, uint16_t len) {
    if (frame == nullptr || len == 0 || len > kMaxFrameLen) return;  // reject, never truncate.
    if (!matchesTarget(frame, len)) return;                          // not our network.

    if (isBeacon(frame, len)) {
        store(handshake_.beacon, frame, len);
        // Empty on a hidden or malformed-SSID network; the beacon bytes are stored either way, so
        // the pcap is byte-exact regardless of whether a display name could be read.
        beaconSsid(frame, len, handshake_.ssid);
        return;
    }

    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame, len, eapolLen);
    if (eapol == nullptr) return;  // a data frame, but not EAPOL.

    const int slot = messageSlot(identifyMessage(eapol, eapolLen));
    if (slot < 0) return;  // EAPOL, but not one of the four pairwise messages.
    store(handshake_.msg[slot], frame, len);
}

void HandshakeCollector::reset() {
    CapturedHandshake fresh;
    std::memcpy(fresh.bssid, handshake_.bssid, sizeof(fresh.bssid));
    fresh.channel = handshake_.channel;
    handshake_ = fresh;
}

}  // namespace sapper
