/**
 * @file cracked_result.h
 * @brief One recovered password as parsed from a wpa-sec `?api&dl=1` line (ADR-0019 decision #3).
 *
 * The value the appliance exists to report: an AP BSSID, its ESSID, and the cracked PSK. It is the
 * unit the pure parser (cracked_result_parser.h) produces, the manifest (cracked_manifest.h) stores
 * per BSSID, and the delta detector (cracked_delta.h) diffs — a plain value type carried between the
 * pure units on the native lane, with no filesystem or network dependency.
 *
 * The buffers match the reference contract measured on hardware (adversary:wpasec_service): an ESSID
 * is at most the 802.11 32-octet SSID plus a terminator, and a password field holds a WPA PSK
 * (8-63 chars) or a 64-hex raw PMK plus a terminator. A field that would overrun these is a malformed
 * line the parser skips and counts, never a silently truncated record (quality bar §3).
 */
#pragma once

#include <cstdint>

namespace sapper {

/// ESSID capacity: 32-octet 802.11 SSID + NUL (matches CapturedHandshake::ssid and the reference).
constexpr size_t kCrackedEssidCap = 33;

/// Password capacity: a 64-hex raw PMK (the longest wpa-sec returns) + NUL. A WPA PSK (<=63) fits too.
constexpr size_t kCrackedPasswordCap = 65;

/// One cracked AP: the BSSID key, its ESSID, and the recovered password. NUL-terminated strings.
struct CrackedResult {
    uint8_t bssid[6] = {0};                 ///< AP BSSID — the per-AP mirror key (ADR-0019 decision #4).
    char essid[kCrackedEssidCap] = {0};     ///< The network name from the cracked capture.
    char password[kCrackedPasswordCap] = {0};  ///< The recovered PSK/PMK, kept verbatim (decision #3).
};

}  // namespace sapper
