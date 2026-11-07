/**
 * @file handshake_collector.cpp
 * @brief Implementation of the pure handshake accumulator (ADR-0009).
 */
#include "net/handshake_collector.h"

#include <cstring>

#include "net/pmkid.h"

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

constexpr size_t kMacLen = 6;  ///< An 802.11 station address, for comparing M1's and M2's clients.

// Whether the stored M1 and M2 belong to one 4-way exchange: the same station and an equal replay
// counter (ADR-0115). The collector keeps only the latest M1 and latest M2 for the target BSSID
// (ADR-0009), so without this a solicited M1 addressed to our spoofed STA pairs with an ambient
// client's M2, and two concurrent exchanges on a busy AP cross their M1/M2 — either way an invalid
// handshake wpa-sec rejects. Read from the whole stored frames in the pure core (no field extracted
// in the collector, ADR-0009 #3 / §4 #8), like the hc22000 synthesis and the PMKID extraction.
bool m1m2FormPair(const CapturedHandshake& hs) {
    if (!hs.has(HandshakeMessage::M1) || !hs.has(HandshakeMessage::M2)) return false;

    // Same station. M1 is AP→client (the client is Addr1), M2 is client→AP (the client is Addr2);
    // stationMac picks the right one by the DS bits. A mismatch means the ANonce (M1) and the MIC
    // (M2) came from different stations (the solicited-M1-vs-ambient-M2 case).
    const uint8_t* m1Sta = stationMac(hs.msg[0].data, hs.msg[0].len);
    const uint8_t* m2Sta = stationMac(hs.msg[1].data, hs.msg[1].len);
    if (m1Sta == nullptr || m2Sta == nullptr) return false;
    if (std::memcmp(m1Sta, m2Sta, kMacLen) != 0) return false;

    // Equal replay counter. M2 echoes M1's counter within one exchange; a mismatch means the two
    // messages are from different handshakes even on the same station. Do NOT loosen this to
    // ANonce-equality to rescue an M1-retransmit-with-bumped-counter pair: that readmits the
    // nonce-corrected/rogue messagepairs and the cross-station mispair this guards against, and such
    // a pair self-corrects anyway when the client answers the retransmit (ADR-0115 Consequences).
    uint16_t m1EapolLen = 0;
    uint16_t m2EapolLen = 0;
    const uint8_t* m1Eapol = locateEapol(hs.msg[0].data, hs.msg[0].len, m1EapolLen);
    const uint8_t* m2Eapol = locateEapol(hs.msg[1].data, hs.msg[1].len, m2EapolLen);
    uint64_t m1Replay = 0;
    uint64_t m2Replay = 0;
    if (!eapolReplayCounter(m1Eapol, m1EapolLen, m1Replay)) return false;
    if (!eapolReplayCounter(m2Eapol, m2EapolLen, m2Replay)) return false;
    return m1Replay == m2Replay;
}

}  // namespace

bool CapturedHandshake::has(HandshakeMessage message) const {
    const int slot = messageSlot(message);
    return slot >= 0 && msg[slot].present();
}

bool CapturedHandshake::isWpaSecValid() const {
    // Beacon plus an M1/M2 pair from one exchange, not merely both present (ADR-0115): a mispaired
    // M1/M2 yields a capture wpa-sec rejects as "no valid handshakes."
    return hasBeacon() && m1m2FormPair(*this);
}

bool CapturedHandshake::isPmkidValid() const {
    if (!hasBeacon() || !has(HandshakeMessage::M1)) return false;
    const CapturedFrame& m1 = msg[0];
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(m1.data, m1.len, eapolLen);
    if (eapol == nullptr) return false;
    uint8_t pmkid[kPmkidLen];
    return extractPmkidFromEapol(eapol, eapolLen, pmkid);
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
        // Build the display SSID in a stack scratch, then publish it to handshake_.ssid in ONE write —
        // never clear-then-fill in place. handshake_.ssid is read lock-free by the live-hunt snapshot on
        // the app task (ADR-0033, §4 #18) while beacons for the current target keep arriving on the
        // driver task; an in-place clear-then-fill would expose a transient empty name (a snapshot caught
        // mid-write would show no SSID). A scratch build writes the same complete value each time, so a
        // concurrent read always sees a consistent name. Empty on a hidden/malformed-SSID network; the
        // beacon bytes are stored either way, so the pcap is byte-exact regardless. Only overwrite on a
        // successful parse, so a later malformed beacon cannot wipe a good name (§4 #10: allocation-free).
        char ssidScratch[33];
        if (beaconSsid(frame, len, ssidScratch)) {
            std::memcpy(handshake_.ssid, ssidScratch, sizeof(handshake_.ssid));
        }
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
    // Value-init ({}) so the beacon/msg frame buffers are cleared, not just their len flags: a plain
    // `CapturedHandshake fresh;` leaves each CapturedFrame::data[] indeterminate (only len has an
    // NSDMI), and `handshake_ = fresh` would then copy that uninitialised memory (cppcheck uninitvar).
    CapturedHandshake fresh{};
    std::memcpy(fresh.bssid, handshake_.bssid, sizeof(fresh.bssid));
    fresh.channel = handshake_.channel;
    handshake_ = fresh;
}

void HandshakeCollector::retarget(const uint8_t targetBssid[6], uint8_t channel) {
    handshake_ = CapturedHandshake{};
    std::memcpy(handshake_.bssid, targetBssid, sizeof(handshake_.bssid));
    handshake_.channel = channel;
}

}  // namespace sapper
