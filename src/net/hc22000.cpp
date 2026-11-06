/**
 * @file hc22000.cpp
 * @brief Pure hashcat WPA*01 (.hc22000) line synthesis (ADR-0108). See hc22000.h.
 */
#include "net/hc22000.h"

#include <cstdint>
#include <cstring>

#include "net/eapol.h"
#include "net/pmkid.h"

namespace sapper {
namespace {

// EAPOL-Key field offsets from the EAPOL header start (what locateEapol returns). Layout: 4-byte EAPOL
// header, descriptor type (1), key info (2), key length (2), replay counter (8), key nonce (32),
// key IV (16), key RSC (8), key ID (8), key MIC (16), key-data length (2). (ADR-0108 Context table.)
constexpr uint16_t kEapolHeaderLen = 4;
constexpr uint16_t kNonceOffset = 17;   ///< Key Nonce: ANonce in M1, SNonce in M2.
constexpr uint16_t kNonceLen = 32;
constexpr uint16_t kMicOffset = 81;     ///< Key MIC (16 bytes), present in M2.
constexpr uint16_t kMicLen = 16;
// Minimum EAPOL length to hold the MIC plus the 2-byte key-data length that follows it. Matches the
// parser's existing floor (eapol.cpp kEapolMinKeyLen) and the hcxpcapngtool expectation.
constexpr uint16_t kEapolMinForMic = kMicOffset + kMicLen + 2;  // 99
constexpr uint16_t kNonceEnd = kNonceOffset + kNonceLen;        // 49: M1 must hold at least this.

// The station (client) MAC in M2. M2 is client→AP (To-DS), so Addr2 is the transmitting station. The
// 802.11 address fields sit at fixed offsets regardless of QoS/HT header extensions (eapol.cpp).
constexpr uint16_t kStaAddrOffset = 10;
constexpr uint16_t kMacLen = 6;

// The addressed station MAC in M1 (Addr1). M1 is AP→client (From-DS), so Addr1 is the destination
// station: our spoofed solicit MAC in the solicited path, a real client in the deauth-forced path
// (ADR-0110 #6). Fixed offset regardless of QoS/HT header extensions, like kStaAddrOffset above.
constexpr uint16_t kDestAddrOffset = 4;

}  // namespace

bool buildHc22000Line(const CapturedHandshake& hs, char* out, size_t outLen) {
    if (out == nullptr || outLen == 0) return false;
    out[0] = '\0';

    // ANonce comes from M1 and the MIC/SNonce/EAPOL from M2, so the pair is replay-counter-consistent
    // (messagepair 00; ADR-0108 decision #2). Both are required — isWpaSecValid already guarantees them
    // for any capture worth sending, so a capture lacking either yields no line rather than a rejectable one.
    if (!hs.has(HandshakeMessage::M1) || !hs.has(HandshakeMessage::M2)) return false;

    // A hidden network has no crackable WPA*01 line (the ESSID is part of PMK derivation). Skip it loudly
    // for pwncrack; wpa-sec can still crack its pcap via its own ESSID discovery (ADR-0108 decision #4).
    // ESSID length is strlen because CapturedHandshake stores the SSID as a C string with no length field
    // (ADR-0009): an SSID with an embedded/leading NUL reads short and is treated as hidden here. A rare,
    // non-destructive upstream data-model limitation (wpa-sec still gets it by pcap), deferred not hacked (LEDGER).
    const size_t ssidLen = std::strlen(hs.ssid);
    if (ssidLen == 0) return false;

    const CapturedFrame& m1 = hs.msg[0];
    const CapturedFrame& m2 = hs.msg[1];
    if (m2.len < kStaAddrOffset + kMacLen) return false;

    uint16_t m1EapolLen = 0;
    const uint8_t* m1Eapol = locateEapol(m1.data, m1.len, m1EapolLen);
    if (m1Eapol == nullptr || m1EapolLen < kNonceEnd) return false;

    uint16_t m2EapolLen = 0;
    const uint8_t* m2Eapol = locateEapol(m2.data, m2.len, m2EapolLen);
    if (m2Eapol == nullptr) return false;

    // Trust the EAPOL length field (header bytes 2..3, big-endian) to trim any trailing 802.11 FCS or
    // padding the sniffer handed us; fall back to the located length if the declared value is implausible.
    const uint16_t declared =
        static_cast<uint16_t>(kEapolHeaderLen + ((m2Eapol[2] << 8) | m2Eapol[3]));
    uint16_t eapolLen =
        (declared >= kEapolMinForMic && declared <= m2EapolLen) ? declared : m2EapolLen;
    if (eapolLen < kEapolMinForMic) return false;
    if (eapolLen > kMaxFrameLen) eapolLen = kMaxFrameLen;  // bounded copy; a frame never exceeds this.

    // Copy the M2 EAPOL and zero its MIC for the EAPOL blob (hashcat recomputes the MIC over the zeroed
    // field and compares it to the separate MIC field below). The real MIC is read from m2Eapol first.
    const uint8_t* realMic = m2Eapol + kMicOffset;
    uint8_t eapolCopy[kMaxFrameLen];
    std::memcpy(eapolCopy, m2Eapol, eapolLen);
    std::memset(eapolCopy + kMicOffset, 0, kMicLen);

    // Bounded appenders: each returns false on any overflow so a partial line is never emitted.
    size_t pos = 0;
    auto appendStr = [&](const char* s) -> bool {
        while (*s) {
            if (pos + 1 >= outLen) return false;  // keep room for the NUL.
            out[pos++] = *s++;
        }
        return true;
    };
    auto appendHex = [&](const uint8_t* d, size_t n) -> bool {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < n; ++i) {
            if (pos + 2 >= outLen) return false;
            out[pos++] = kHex[d[i] >> 4];
            out[pos++] = kHex[d[i] & 0x0F];
        }
        return true;
    };

    const bool ok =
        appendStr("WPA*01*") &&
        appendHex(realMic, kMicLen) && appendStr("*") &&
        appendHex(hs.bssid, kMacLen) && appendStr("*") &&
        appendHex(m2.data + kStaAddrOffset, kMacLen) && appendStr("*") &&
        appendHex(reinterpret_cast<const uint8_t*>(hs.ssid), ssidLen) && appendStr("*") &&
        appendHex(m1Eapol + kNonceOffset, kNonceLen) && appendStr("*") &&
        appendHex(eapolCopy, eapolLen) &&
        // messagepair 00: M1 ANonce + M2 MIC/SNonce, replay counters matched (ADR-0108 decision #2).
        appendStr("*00");
    if (!ok || pos >= outLen) {
        out[0] = '\0';
        return false;
    }
    out[pos] = '\0';
    return true;
}

bool buildPmkidHc22000Line(const CapturedHandshake& hs, char* out, size_t outLen) {
    if (out == nullptr || outLen == 0) return false;
    out[0] = '\0';

    if (!hs.has(HandshakeMessage::M1)) return false;

    // A hidden network has no crackable line (the ESSID is part of PMK derivation), like WPA*01;
    // an embedded/leading-NUL SSID reads short here and is treated as hidden (same LEDGER limitation).
    const size_t ssidLen = std::strlen(hs.ssid);
    if (ssidLen == 0) return false;

    const CapturedFrame& m1 = hs.msg[0];
    if (m1.len < kDestAddrOffset + kMacLen) return false;  // too short to hold the STA (Addr1).

    uint16_t m1EapolLen = 0;
    const uint8_t* m1Eapol = locateEapol(m1.data, m1.len, m1EapolLen);
    if (m1Eapol == nullptr) return false;

    uint8_t pmkid[kPmkidLen];
    if (!extractPmkidFromEapol(m1Eapol, m1EapolLen, pmkid)) return false;  // no PMKID KDE: no line.

    // Bounded appenders: the second use of the WPA*01 pattern above, duplicated at two per the rule
    // of three (a third hashline type would extract a shared file-local helper, LEDGER). Each returns
    // false on any overflow so a partial line is never emitted.
    size_t pos = 0;
    auto appendStr = [&](const char* s) -> bool {
        while (*s) {
            if (pos + 1 >= outLen) return false;  // keep room for the NUL.
            out[pos++] = *s++;
        }
        return true;
    };
    auto appendHex = [&](const uint8_t* d, size_t n) -> bool {
        static const char kHex[] = "0123456789abcdef";
        for (size_t i = 0; i < n; ++i) {
            if (pos + 2 >= outLen) return false;
            out[pos++] = kHex[d[i] >> 4];
            out[pos++] = kHex[d[i] & 0x0F];
        }
        return true;
    };

    const bool ok =
        appendStr("WPA*02*") &&
        appendHex(pmkid, kPmkidLen) && appendStr("*") &&
        appendHex(hs.bssid, kMacLen) && appendStr("*") &&
        appendHex(m1.data + kDestAddrOffset, kMacLen) && appendStr("*") &&
        appendHex(reinterpret_cast<const uint8_t*>(hs.ssid), ssidLen) &&
        // ANONCE, EAPOL and messagepair are all empty for a PMKID line (no 4-way).
        appendStr("***");
    if (!ok || pos >= outLen) {
        out[0] = '\0';
        return false;
    }
    out[pos] = '\0';
    return true;
}

}  // namespace sapper
