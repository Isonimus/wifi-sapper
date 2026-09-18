/**
 * @file eapol.cpp
 * @brief Implementation of the pure 802.11 / EAPOL frame readers (ADR-0009).
 */
#include "net/eapol.h"

#include <cstring>

namespace sapper {
namespace {

// 802.11 frame geometry. The MAC header is 24 bytes for the frames we read (no QoS/HT control in
// beacons or the EAPOL data frames of interest); addresses sit at fixed offsets within it.
constexpr uint16_t kMacHeaderLen = 24;
constexpr uint16_t kAddr1Offset = 4;
constexpr uint16_t kAddr2Offset = 10;
constexpr uint16_t kAddr3Offset = 16;
constexpr uint16_t kBssidLen = 6;

// Frame Control octet 0: bits 2-3 type, bits 4-7 subtype; bits 0-1 are the protocol version.
constexpr uint8_t kFcTypeSubtypeMask = 0xFC;  ///< Everything but the version bits.
constexpr uint8_t kFcBeacon = 0x80;           ///< Management (type 0), beacon (subtype 8).

// Frame Control octet 1: the DS bits decide where the BSSID lives.
constexpr uint8_t kFcToDs = 0x01;
constexpr uint8_t kFcFromDs = 0x02;

// Beacon fixed body after the MAC header: timestamp(8) + interval(2) + capability(2) = 12 bytes,
// then the tagged-parameter IE list begins. Each IE is [element id][length][data...].
constexpr uint16_t kBeaconFixedLen = 12;
constexpr uint8_t kIeSsid = 0x00;  ///< SSID element id.
constexpr size_t kMaxSsidOctets = 32;

// LLC/SNAP header carrying EAPOL: AA AA 03 00 00 00 88 8E. The SNAP header sits at a variable
// offset because the MAC header is 24-30 bytes, so we scan a small window for it.
constexpr uint8_t kLlcSnapEapol[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
constexpr uint16_t kSnapScanStart = 24;
constexpr uint16_t kSnapScanEnd = 40;  ///< Exclusive upper bound on the SNAP-header start offset.

// EAPOL-Key layout. The EAPOL header is Version(1)+Type(1)+Length(2); the Key frame follows.
constexpr uint16_t kEapolMinKeyLen = 99;  ///< Shortest valid EAPOL-Key frame.
constexpr uint16_t kEapolTypeOffset = 1;
constexpr uint8_t kEapolTypeKey = 0x03;
constexpr uint16_t kKeyFrameOffset = 4;      ///< Key descriptor byte, relative to the EAPOL header.
constexpr uint8_t kKeyDescWpa2 = 0x02;
constexpr uint8_t kKeyDescWpa = 0xFE;

// Key Information bit masks (the 2-byte field at keyFrame[1..2], big-endian).
constexpr uint16_t kKeyInfoDescVerMask = 0x0007;
constexpr uint16_t kKeyInfoPairwise = 0x0008;
constexpr uint16_t kKeyInfoInstall = 0x0040;
constexpr uint16_t kKeyInfoAck = 0x0080;
constexpr uint16_t kKeyInfoMic = 0x0100;
constexpr uint16_t kKeyInfoSecure = 0x0200;

}  // namespace

bool isBeacon(const uint8_t* frame, uint16_t len) {
    if (frame == nullptr || len < 1) return false;
    return (frame[0] & kFcTypeSubtypeMask) == kFcBeacon;
}

const uint8_t* frameBssid(const uint8_t* frame, uint16_t len) {
    // The three addresses sit at fixed offsets even for a QoS/HT-Control-extended header: those
    // variable fields come *after* Sequence Control, so Addr1-Addr3 never move. No header-length
    // computation is needed here (that is locateEapol's problem, and it scans rather than assumes).
    if (frame == nullptr || len < kAddr3Offset + kBssidLen) return nullptr;
    const bool toDs = (frame[1] & kFcToDs) != 0;
    const bool fromDs = (frame[1] & kFcFromDs) != 0;
    if (toDs && !fromDs) return frame + kAddr1Offset;   // client → AP: BSSID in Addr1.
    if (!toDs && fromDs) return frame + kAddr2Offset;    // AP → client: BSSID in Addr2.
    return frame + kAddr3Offset;                         // mgmt/beacon or IBSS/WDS: BSSID in Addr3.
}

bool beaconSsid(const uint8_t* frame, uint16_t len, char (&out)[33]) {
    out[0] = '\0';
    if (frame == nullptr) return false;
    // The IE list starts at a fixed offset: a beacon always has a 24-byte MAC header (no QoS/HTC
    // applies to management frames) followed by the 12-byte fixed body. Walk from there; `pos`
    // indexes the [id][len] pair, and an IE that would run past the frame end is malformed, so we
    // stop rather than read out of bounds (fail loud, no partial read).
    uint16_t pos = kMacHeaderLen + kBeaconFixedLen;
    while (pos + 2 <= len) {
        const uint8_t id = frame[pos];
        const uint8_t ieLen = frame[pos + 1];
        if (pos + 2 + ieLen > len) return false;  // IE claims more bytes than the frame holds.
        if (id == kIeSsid) {
            // An SSID element over the 32-octet 802.11 maximum is itself malformed. Reject rather
            // than clamp to 32: a silently truncated network name is a masked error (quality bar §3).
            if (ieLen > kMaxSsidOctets) return false;
            std::memcpy(out, frame + pos + 2, ieLen);
            out[ieLen] = '\0';
            return true;  // A zero-length SSID (hidden network) is a valid, empty result.
        }
        pos += 2 + ieLen;
    }
    return false;
}

const uint8_t* locateEapol(const uint8_t* frame, uint16_t len, uint16_t& eapolLen) {
    eapolLen = 0;
    if (frame == nullptr) return nullptr;
    for (uint16_t i = kSnapScanStart; i + sizeof(kLlcSnapEapol) <= len && i < kSnapScanEnd; ++i) {
        if (std::memcmp(frame + i, kLlcSnapEapol, sizeof(kLlcSnapEapol)) == 0) {
            const uint16_t start = i + sizeof(kLlcSnapEapol);
            eapolLen = len - start;
            return frame + start;
        }
    }
    return nullptr;
}

EapolKeyInfo parseKeyInfo(uint16_t keyInfo) {
    EapolKeyInfo info{};
    info.keyDescVer = static_cast<uint8_t>(keyInfo & kKeyInfoDescVerMask);
    info.pairwise = (keyInfo & kKeyInfoPairwise) != 0;
    info.install = (keyInfo & kKeyInfoInstall) != 0;
    info.keyAck = (keyInfo & kKeyInfoAck) != 0;
    info.keyMic = (keyInfo & kKeyInfoMic) != 0;
    info.secure = (keyInfo & kKeyInfoSecure) != 0;
    return info;
}

HandshakeMessage identifyMessage(const uint8_t* eapol, uint16_t eapolLen) {
    if (eapol == nullptr || eapolLen < kEapolMinKeyLen) return HandshakeMessage::Unknown;
    if (eapol[kEapolTypeOffset] != kEapolTypeKey) return HandshakeMessage::Unknown;

    const uint8_t* keyFrame = eapol + kKeyFrameOffset;
    const uint8_t descType = keyFrame[0];
    if (descType != kKeyDescWpa2 && descType != kKeyDescWpa) return HandshakeMessage::Unknown;

    const uint16_t keyInfo = static_cast<uint16_t>((keyFrame[1] << 8) | keyFrame[2]);
    const EapolKeyInfo info = parseKeyInfo(keyInfo);
    if (!info.pairwise) return HandshakeMessage::Unknown;  // group-key rekey, not a 4-way message.

    // The four pairwise messages are distinguished purely by their Ack/MIC/Install/Secure flags.
    if (info.keyAck && !info.keyMic && !info.install && !info.secure) return HandshakeMessage::M1;
    if (!info.keyAck && info.keyMic && !info.install && !info.secure) return HandshakeMessage::M2;
    if (info.keyAck && info.keyMic && info.install && info.secure) return HandshakeMessage::M3;
    if (!info.keyAck && info.keyMic && !info.install && info.secure) return HandshakeMessage::M4;
    return HandshakeMessage::Unknown;
}

}  // namespace sapper
