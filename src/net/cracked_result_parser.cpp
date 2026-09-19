/**
 * @file cracked_result_parser.cpp
 * @brief Implementation of the pure wpa-sec cracked-line parser (ADR-0019 decision #3).
 */
#include "net/cracked_result_parser.h"

#include <cstring>

namespace sapper {

namespace {

constexpr size_t kBssidHexChars = 12;  ///< 6 octets, two hex nibbles each, no separators in the field.

/// Convert one hex nibble to its value, or return false if @p c is not a hex digit.
bool hexNibble(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') { out = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

/// Parse exactly 12 hex nibbles (no separators, as the download field is written) into 6 octets.
/// Returns false on any non-hex character or a wrong length — an unparseable BSSID is malformed.
bool parseBssidField(std::string_view field, uint8_t out[6]) {
    if (field.size() != kBssidHexChars) return false;
    for (size_t i = 0; i < 6; ++i) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!hexNibble(field[2 * i], hi) || !hexNibble(field[2 * i + 1], lo)) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

ParseLineResult parseCrackedLine(std::string_view line, CrackedResult& out) {
    // Tolerate an HTTP CRLF (or bare LF) terminator a streaming caller may leave on the line.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    if (line.empty()) return ParseLineResult::Empty;

    // Split on the first three colons only; the remainder is the password, verbatim (decision #3).
    const size_t colon1 = line.find(':');
    if (colon1 == std::string_view::npos) return ParseLineResult::Malformed;
    const size_t colon2 = line.find(':', colon1 + 1);
    if (colon2 == std::string_view::npos) return ParseLineResult::Malformed;
    const size_t colon3 = line.find(':', colon2 + 1);
    if (colon3 == std::string_view::npos) return ParseLineResult::Malformed;

    const std::string_view apBssid = line.substr(0, colon1);
    // line[colon1+1 .. colon2) is the client BSSID — not part of the per-AP mirror, so it is ignored.
    const std::string_view essid = line.substr(colon2 + 1, colon3 - (colon2 + 1));
    const std::string_view password = line.substr(colon3 + 1);

    uint8_t bssid[6] = {0};
    if (!parseBssidField(apBssid, bssid)) return ParseLineResult::Malformed;

    // Reject empty or over-long fields rather than storing a truncated record (quality bar §3).
    if (essid.empty() || essid.size() >= kCrackedEssidCap) return ParseLineResult::Malformed;
    if (password.empty() || password.size() >= kCrackedPasswordCap) return ParseLineResult::Malformed;

    // Everything validated: populate @p out only now, so a malformed line never leaves a partial record.
    std::memcpy(out.bssid, bssid, sizeof(out.bssid));
    std::memcpy(out.essid, essid.data(), essid.size());
    out.essid[essid.size()] = '\0';
    std::memcpy(out.password, password.data(), password.size());
    out.password[password.size()] = '\0';
    return ParseLineResult::Ok;
}

}  // namespace sapper
