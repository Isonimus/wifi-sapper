/**
 * @file pcap_reader.cpp
 * @brief Implementation of pcap-to-CapturedHandshake parsing (ADR-0109 decision 2). See pcap_reader.h.
 *
 * The exact inverse of pcap.cpp: that writer emits the global header then Beacon/M1-M4 records; this reader
 * validates the header and classifies each record's frame back into the handshake. Both targets are
 * little-endian and the writer wrote host-order headers under the LE magic, so the reader decodes the
 * header integers little-endian explicitly (not a packed struct cast) to stay correct independent of the
 * host and free of alignment assumptions on the record stream.
 */
#include "net/pcap_reader.h"

#include <cstring>

#include "net/eapol.h"
#include "net/pcap.h"

namespace sapper {
namespace {

// Decode a little-endian uint32 at @p p; the caller has already bounds-checked the 4 bytes.
uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Classify one record's frame into the handshake under construction, reusing only the pure capture
// readers so no new 802.11/EAPOL interpretation is added (ADR-0109 decision 2, §4 #8). @p frameLen is
// already bounded to kMaxFrameLen by the caller, so every copy below fits a CapturedFrame.
void absorbFrame(const uint8_t* frame, uint16_t frameLen, CapturedHandshake& out) {
    if (isBeacon(frame, frameLen)) {
        const uint8_t* bssid = frameBssid(frame, frameLen);
        if (bssid != nullptr) std::memcpy(out.bssid, bssid, 6);
        beaconSsid(frame, frameLen, out.ssid);      // leaves out.ssid empty + NUL on a missing/hidden SSID.
        out.channel = beaconChannel(frame, frameLen);  // AP's DS-param channel; 0 if the beacon omits it.
        std::memcpy(out.beacon.data, frame, frameLen);
        out.beacon.len = frameLen;
        return;
    }
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame, frameLen, eapolLen);
    if (eapol == nullptr) return;  // not a beacon and not EAPOL (a foreign/data frame): drop it.
    const HandshakeMessage message = identifyMessage(eapol, eapolLen);
    if (message == HandshakeMessage::Unknown) return;  // group-key or unrecognised: not one of M1-M4.
    const int slot = static_cast<int>(message) - static_cast<int>(HandshakeMessage::M1);
    std::memcpy(out.msg[slot].data, frame, frameLen);
    out.msg[slot].len = frameLen;
}

}  // namespace

bool parseHandshakePcap(const uint8_t* bytes, size_t len, CapturedHandshake& out) {
    out = CapturedHandshake{};  // reset up front: no partial handshake survives any failure path below.
    if (bytes == nullptr) return false;
    if (len < kPcapGlobalHeaderLen) return false;  // too short to hold even the global header.

    // The global header pins byte order (magic, offset 0) and link type (network, offset 20). A mismatch
    // means this is not our serializer's output (a foreign or corrupt file): fail loud, never guess.
    if (readLe32(bytes) != kPcapMagicMicroseconds) return false;
    if (readLe32(bytes + 20) != kPcapLinkTypeIeee80211) return false;

    size_t cursor = kPcapGlobalHeaderLen;
    while (cursor < len) {
        if (cursor + kPcapRecordHeaderLen > len) return false;  // a trailing partial record header.
        const uint32_t inclLen = readLe32(bytes + cursor + 8);  // captured-bytes field of the record.
        cursor += kPcapRecordHeaderLen;
        // A frame longer than a CapturedFrame can hold, or one that runs past the buffer, is corruption.
        // Checking the kMaxFrameLen bound first also keeps cursor + inclLen from overflowing size_t on a
        // 32-bit device. A truncated frame in a capture is silently wrong (quality bar §3): fail loud.
        if (inclLen > kMaxFrameLen) return false;
        if (cursor + inclLen > len) return false;
        absorbFrame(bytes + cursor, static_cast<uint16_t>(inclLen), out);
        cursor += inclLen;
    }

    // Only a set good enough to upload (beacon + M1 + M2) is a success; anything less is reset so no
    // caller derives a hashline from a partial handshake (ADR-0109 decision 2).
    if (!out.isWpaSecValid()) {
        out = CapturedHandshake{};
        return false;
    }
    return true;
}

}  // namespace sapper
