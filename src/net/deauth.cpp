/**
 * @file deauth.cpp
 * @brief Pure deauth/disassoc frame construction (ADR-0027). No radio, no Serial — native lane.
 */
#include "net/deauth.h"

#include <cstring>

namespace sapper {

const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

namespace {
// Named byte offsets into the fixed 26-byte management frame (ADR-0027), so the layout reads as the
// 802.11 header it is rather than a run of magic indices. The fields left zero — FC flags (byte 1),
// duration (bytes 2-3), and sequence control (bytes 22-23) — are zeroed by the memset below and so
// need no named offset or explicit write.
constexpr size_t kOffFcSubtype = 0;   // frame-control byte 0: type + subtype.
constexpr size_t kOffAddr1 = 4;       // destination (6 bytes).
constexpr size_t kOffAddr2 = 10;      // source (6 bytes) — the spoofed AP.
constexpr size_t kOffAddr3 = 16;      // BSSID (6 bytes).
constexpr size_t kOffReason = 24;     // reason code (2 bytes, little-endian).
constexpr size_t kMacLen = 6;
}  // namespace

size_t buildDeauthFrame(uint8_t* out, size_t cap, ManagementSubtype subtype,
                        const uint8_t dest[6], const uint8_t bssid[6], DeauthReason reason) {
    if (cap < kDeauthFrameLen) return 0;  // refuse, never truncate (quality-bar §3).

    std::memset(out, 0, kDeauthFrameLen);  // FC flags, duration, and sequence are all zero.
    out[kOffFcSubtype] = static_cast<uint8_t>(subtype);
    std::memcpy(&out[kOffAddr1], dest, kMacLen);
    std::memcpy(&out[kOffAddr2], bssid, kMacLen);  // spoof the source as the AP.
    std::memcpy(&out[kOffAddr3], bssid, kMacLen);

    const uint16_t code = static_cast<uint16_t>(reason);
    out[kOffReason] = static_cast<uint8_t>(code & 0xFF);
    out[kOffReason + 1] = static_cast<uint8_t>((code >> 8) & 0xFF);

    return kDeauthFrameLen;
}

}  // namespace sapper
