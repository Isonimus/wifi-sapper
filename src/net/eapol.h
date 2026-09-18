/**
 * @file eapol.h
 * @brief Pure 802.11 / EAPOL frame reading for handshake capture (ADR-0009).
 *
 * The hardware-free "read one frame" half of the capture core: no `esp_wifi`, no `WiFi`, no
 * `Serial`. Given a raw 802.11 frame as a promiscuous sniffer would hand it over, it answers the
 * questions the accumulator needs — is this a beacon, whose BSSID is it, where is the EAPOL
 * payload, and which 4-way-handshake message is it. Host-unit-tested (ADR-0004 lane 1); the
 * constants and bit logic are carried from `../adversary/src/modules/capture/handshake_capture.cpp`
 * and rebuilt as free functions so a test calls them with no instance (ADR-0009 decision #1).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// Which message of the WPA/WPA2 4-way handshake an EAPOL-Key frame carries. `Unknown` covers a
/// group-key frame, a malformed frame, or anything that is not one of the four pairwise messages.
enum class HandshakeMessage : uint8_t {
    Unknown = 0,
    M1,  ///< AP → client: ANonce (Pairwise + Ack).
    M2,  ///< Client → AP: SNonce + MIC (Pairwise + MIC).
    M3,  ///< AP → client: ANonce + MIC + Install (Pairwise + Ack + MIC + Install + Secure).
    M4,  ///< Client → AP: MIC confirmation (Pairwise + MIC + Secure).
};

/// The decoded EAPOL-Key **Key Information** field (2 bytes, big-endian). Only the bits the
/// message-identification logic reads are surfaced; the rest of the field is not consulted.
struct EapolKeyInfo {
    uint8_t keyDescVer;  ///< Key descriptor version (bits 0-2): 1=HMAC-MD5, 2=HMAC-SHA1, 3=AES-CMAC.
    bool pairwise;       ///< Pairwise key (vs group key).
    bool install;        ///< Install-key flag.
    bool keyAck;         ///< Key ACK (AP → client).
    bool keyMic;         ///< Message carries a MIC.
    bool secure;         ///< Secure bit set.
};

// The largest 802.11 frame the capture core stores or inspects. Matches the reference's largest
// buffer (M3 with encrypted key data, and its beacon cap). A frame past this is rejected, never
// truncated — a truncated frame in a pcap is a silently corrupt capture (quality bar §3).
constexpr size_t kMaxFrameLen = 512;

/// Whether @p frame is a beacon management frame (type 0 / subtype 8). Needs the first FC octet;
/// the two low bits are the protocol version and are masked off.
bool isBeacon(const uint8_t* frame, uint16_t len);

/**
 * @brief Pointer to the 6-byte BSSID inside @p frame, or nullptr if @p frame is too short.
 *
 * Which address field holds the BSSID depends on the To-DS / From-DS bits (FC octet 1): a
 * client→AP data frame carries it in Addr1, an AP→client frame in Addr2, and a management frame
 * (To-DS=From-DS=0, e.g. a beacon) or IBSS frame in Addr3.
 */
const uint8_t* frameBssid(const uint8_t* frame, uint16_t len);

/**
 * @brief Copy a beacon's SSID into @p out (33 bytes: 32 octets + NUL), returning true if an SSID
 *        element was present.
 *
 * Walks the tagged-parameter IE list after the fixed beacon body. A hidden SSID (zero-length
 * element) yields an empty string and still returns true; a frame with no SSID IE or a malformed
 * IE list returns false. @p out is always NUL-terminated.
 */
bool beaconSsid(const uint8_t* frame, uint16_t len, char (&out)[33]);

/**
 * @brief Locate the EAPOL payload inside an 802.11 data frame.
 *
 * Scans for the LLC/SNAP header `AA AA 03 00 00 00 88 8E` (the 802.11 header is 24-30 bytes, so
 * the SNAP header sits at a variable offset). Returns a pointer to the first EAPOL byte and writes
 * its length to @p eapolLen, or nullptr if the SNAP/EAPOL EtherType is not found.
 */
const uint8_t* locateEapol(const uint8_t* frame, uint16_t len, uint16_t& eapolLen);

/// Decode the Key Information bit field.
EapolKeyInfo parseKeyInfo(uint16_t keyInfo);

/**
 * @brief Identify which handshake message an EAPOL payload carries (ADR-0009 decision #1).
 *
 * @p eapol points at the EAPOL header (as returned by locateEapol). Returns `Unknown` for a frame
 * too short to be an EAPOL-Key, a non-Key EAPOL type, an unrecognised key-descriptor type, or a
 * Key-Information bit combination that is not one of the four pairwise messages.
 */
HandshakeMessage identifyMessage(const uint8_t* eapol, uint16_t eapolLen);

}  // namespace sapper
