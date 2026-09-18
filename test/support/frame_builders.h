/**
 * @file frame_builders.h
 * @brief Header-only 802.11 frame builders for the capture-core native tests (ADR-0009).
 *
 * Assemble the minimal beacon and EAPOL-Key data frames the pure parser reads, so each test states
 * the frame it feeds in named terms (which BSSID, which handshake message) rather than a wall of
 * hex. Shared by test_eapol, test_handshake_collector, and test_pcap (rule of three). Header-only
 * and included by relative path, so it needs no extra include-path or build-src wiring.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sapper_test {

// EAPOL-Key Information bit values, big-endian in the frame. The four pairwise messages differ only
// in these flags (see identifyMessage): M1 Ack; M2 MIC; M3 Ack+MIC+Install+Secure; M4 MIC+Secure.
constexpr uint16_t kKiPairwise = 0x0008;
constexpr uint16_t kKiInstall = 0x0040;
constexpr uint16_t kKiAck = 0x0080;
constexpr uint16_t kKiMic = 0x0100;
constexpr uint16_t kKiSecure = 0x0200;

constexpr uint16_t kKeyInfoM1 = kKiPairwise | kKiAck;
constexpr uint16_t kKeyInfoM2 = kKiPairwise | kKiMic;
constexpr uint16_t kKeyInfoM3 = kKiPairwise | kKiAck | kKiMic | kKiInstall | kKiSecure;
constexpr uint16_t kKeyInfoM4 = kKiPairwise | kKiMic | kKiSecure;

/// Build a beacon frame advertising @p ssid for @p bssid. A hidden network is an empty @p ssid.
inline std::vector<uint8_t> buildBeacon(const uint8_t bssid[6], const std::string& ssid) {
    std::vector<uint8_t> f(24, 0);
    f[0] = 0x80;  // FC: management / beacon.
    std::memset(&f[4], 0xFF, 6);          // Addr1: broadcast destination.
    std::memcpy(&f[10], bssid, 6);        // Addr2: source = BSSID.
    std::memcpy(&f[16], bssid, 6);        // Addr3: BSSID.
    f.insert(f.end(), 12, 0);             // Fixed body: timestamp + interval + capability.
    f.push_back(0x00);                    // SSID element id.
    f.push_back(static_cast<uint8_t>(ssid.size()));
    f.insert(f.end(), ssid.begin(), ssid.end());
    return f;
}

/// Build an EAPOL-Key data frame for @p bssid carrying @p keyInfo. @p fromAp true = AP→client
/// (From-DS, BSSID in Addr2); false = client→AP (To-DS, BSSID in Addr1).
inline std::vector<uint8_t> buildEapol(const uint8_t bssid[6], const uint8_t client[6],
                                       uint16_t keyInfo, bool fromAp) {
    std::vector<uint8_t> f(24, 0);
    f[0] = 0x08;  // FC: data frame.
    f[1] = fromAp ? 0x02 : 0x01;  // From-DS or To-DS.
    if (fromAp) {
        std::memcpy(&f[4], client, 6);   // Addr1: destination = client.
        std::memcpy(&f[10], bssid, 6);   // Addr2: BSSID.
    } else {
        std::memcpy(&f[4], bssid, 6);    // Addr1: BSSID.
        std::memcpy(&f[10], client, 6);  // Addr2: source = client.
    }
    std::memcpy(&f[16], bssid, 6);       // Addr3.

    // LLC/SNAP header carrying the EAPOL EtherType, at offset 24.
    const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    f.insert(f.end(), snap, snap + 8);

    // EAPOL-Key body, exactly 99 bytes (the parser's minimum). Header: version, type=Key(3),
    // length; then key descriptor(0x02 = WPA2) and the Key Information field.
    std::vector<uint8_t> eapol(99, 0);
    eapol[0] = 0x02;                                       // EAPOL version.
    eapol[1] = 0x03;                                       // EAPOL type: Key.
    eapol[3] = static_cast<uint8_t>(99 - 4);              // EAPOL body length (95).
    eapol[4] = 0x02;                                       // Key descriptor type: WPA2.
    eapol[5] = static_cast<uint8_t>(keyInfo >> 8);         // Key Information (big-endian).
    eapol[6] = static_cast<uint8_t>(keyInfo & 0xFF);
    f.insert(f.end(), eapol.begin(), eapol.end());
    return f;
}

}  // namespace sapper_test
