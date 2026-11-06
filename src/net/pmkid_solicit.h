/**
 * @file pmkid_solicit.h
 * @brief Pure 802.11 frame builders for clientless-PMKID solicitation (ADR-0110).
 *
 * The blind pair that draws an EAPOL M1 with a PMKID KDE from a PMK-caching WPA2-PSK AP with no
 * client present: an open-system authentication request, then (the ~25 ms wait and the repeat are
 * the device shell's job, slice 2) a WPA2-PSK association request carrying an RSN IE (CCMP + AKM
 * PSK), both from a spoofed locally-administered station MAC. The promiscuous RX path then records
 * the PMKID keyed on that STA (the correct hashcat STA for this path). The built bytes are put on
 * the air through `RawTransmitter` (§4 #15); these builders carry no radio and no protocol state.
 *
 * Carried from `../adversary/src/utils/pmkid_solicit.h`: the exact bytes here are the frames that
 * drew M1 on-device on 2026-11-04 (adversary:ADR-0138). A reordered field, a swapped cipher/AKM
 * suite, or a mis-sized SSID element silently stops soliciting M1 on hardware, so the byte layout
 * is the load-bearing artifact (test_pmkid_solicit guards it). Host-unit-tested (ADR-0004 lane 1).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// Largest frame either builder emits is the association request with a 32-byte SSID:
/// header(24) + capability(2) + listen(2) + SSID(2+32) + rates(2+8) + RSN(2+20) = 94. Callers size
/// their TX buffer to this so a build is never bounds-checked at the call site.
constexpr uint16_t kSolicitFrameBufLen = 128;
/// Open-system authentication request on-wire length (no variable fields).
constexpr uint16_t kSolicitAuthFrameLen = 30;
/// 802.11 SSID element cap.
constexpr uint8_t kSolicitSsidMaxLen = 32;

/**
 * @brief The spoofed station MAC solicitation defaults to.
 *
 * First octet 0x02 sets the locally-administered bit and clears the multicast bit, so a PMKID
 * recorded against it is unambiguously the product of solicitation, not a real client's, and the
 * driver does not claim the AP's replies while promiscuous still delivers them. The proven
 * 2026-11-04 value; a per-hunt provisioned override is a later concern (LEDGER). Returns a pointer
 * to a static 6-byte array.
 */
const uint8_t* defaultSolicitStaMac();

/**
 * @brief Build an open-system authentication request (transaction sequence 1).
 *
 * @param buf    output buffer.
 * @param bufLen capacity of @p buf.
 * @param bssid  target AP BSSID (addr1 and addr3).
 * @param src    spoofed station MAC (addr2).
 * @return the frame length (kSolicitAuthFrameLen), or 0 if any pointer is null or @p buf is too
 *         small, so the caller fails loud instead of transmitting a short frame.
 */
uint16_t buildAuthRequest(uint8_t* buf, size_t bufLen, const uint8_t* bssid, const uint8_t* src);

/**
 * @brief Build a WPA2-PSK association request: SSID + supported rates + RSN IE (CCMP/CCMP/PSK).
 *
 * @param buf    output buffer.
 * @param bufLen capacity of @p buf.
 * @param bssid  target AP BSSID (addr1 and addr3).
 * @param src    spoofed station MAC (addr2).
 * @param ssid   target SSID (clamped to kSolicitSsidMaxLen octets).
 * @return the frame length, or 0 if any pointer is null or @p buf cannot hold the built frame.
 */
uint16_t buildAssocRequest(uint8_t* buf, size_t bufLen, const uint8_t* bssid, const uint8_t* src,
                           const char* ssid);

}  // namespace sapper
