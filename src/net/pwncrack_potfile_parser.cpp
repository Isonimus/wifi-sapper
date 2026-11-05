/**
 * @file pwncrack_potfile_parser.cpp
 * @brief Implementation of the pure pwncrack potfile-line parser (ADR-0109 decision #5).
 */
#include "net/pwncrack_potfile_parser.h"

#include <cstring>

namespace sapper {

namespace {

constexpr size_t kBssidHexChars = 12;  ///< 6 octets, two hex nibbles each.

/// Convert one hex nibble to its value, or return false if @p c is not a hex digit.
bool hexNibble(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') { out = static_cast<uint8_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<uint8_t>(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<uint8_t>(c - 'A' + 10); return true; }
    return false;
}

/// Parse the pwncrack BSSID field into 6 octets. The field sits between two colons so it never contains
/// one, but the potfile has been seen to write the MAC either bare (`aabbccddeeff`) or hyphen-separated
/// (`aa-bb-cc-dd-ee-ff`); we accept both by skipping a single '-' between octets and requiring exactly
/// 12 hex nibbles. Anything else (non-hex, wrong count) is a malformed BSSID, so the line is rejected
/// rather than stored against a zeroed/partial key (quality bar §3). Mirrors the sibling Adversary's
/// `parseBssid` tolerance; the stricter wpa-sec field (bare 12-hex only) is parsed separately (#5).
bool parsePwncrackBssidField(std::string_view field, uint8_t out[6]) {
    uint8_t nibbles[kBssidHexChars];
    size_t n = 0;
    for (char c : field) {
        if (c == '-') continue;  // tolerate a hyphen-grouped MAC.
        if (n >= kBssidHexChars) return false;  // more hex than a MAC holds.
        if (!hexNibble(c, nibbles[n])) return false;
        ++n;
    }
    if (n != kBssidHexChars) return false;
    for (size_t i = 0; i < 6; ++i) {
        out[i] = static_cast<uint8_t>((nibbles[2 * i] << 4) | nibbles[2 * i + 1]);
    }
    return true;
}

}  // namespace

ParseLineResult parsePwncrackPotfileLine(std::string_view line, CrackedResult& out) {
    // Tolerate an HTTP CRLF (or bare LF) terminator and trailing spaces a streaming caller may leave on.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return ParseLineResult::Empty;

    // Locate the first four colons: BSSID is field index 1, ESSID index 3, password everything after the
    // fourth colon (index 4), kept verbatim so a password containing ':' survives (ADR-0109 #5).
    const size_t colon0 = line.find(':');
    if (colon0 == std::string_view::npos) return ParseLineResult::Malformed;
    const size_t colon1 = line.find(':', colon0 + 1);
    if (colon1 == std::string_view::npos) return ParseLineResult::Malformed;
    const size_t colon2 = line.find(':', colon1 + 1);
    if (colon2 == std::string_view::npos) return ParseLineResult::Malformed;
    const size_t colon3 = line.find(':', colon2 + 1);
    if (colon3 == std::string_view::npos) return ParseLineResult::Malformed;

    // line[0 .. colon0) is field index 0 (an auxiliary name field, ignored; the authoritative ESSID is
    // index 3). line[colon2+1 .. colon3) is that ESSID; line[colon3+1 ..] is the password.
    const std::string_view apBssid = line.substr(colon0 + 1, colon1 - (colon0 + 1));
    const std::string_view essid = line.substr(colon2 + 1, colon3 - (colon2 + 1));
    const std::string_view password = line.substr(colon3 + 1);

    uint8_t bssid[6] = {0};
    if (!parsePwncrackBssidField(apBssid, bssid)) return ParseLineResult::Malformed;

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
