/**
 * @file pwncrack_potfile_parser.h
 * @brief Pure parser for one pwncrack `download_potfile_script` line into a CrackedResult
 *        (ADR-0109 decision #5).
 *
 * pwncrack's results download is a colon-delimited potfile, one cracked AP per line, whose field order
 * differs from wpa-sec's `?api&dl=1` body: the AP BSSID is field **index 1**, the ESSID is field
 * **index 3**, and the password is **everything after the fourth colon** (index 4), kept verbatim. The
 * layout was verified against the live site and the community `pwncrack-addons` plugin (LEDGER
 * 2026-11-02) and matches the sibling Adversary's working parser (`adversary:pwncrack_service`).
 *
 * ADR-0109 decision #5 chose a **sibling parser** here rather than parameterising the wpa-sec parser:
 * the two layouts differ (wpa-sec is BSSID 0 / essid 2 / password after the third colon; pwncrack is
 * BSSID 1 / essid 3 / password after the fourth), and at two backends the duplicate-don't-abstract rule
 * (quality bar §3, rule of three) keeps each parse a flat, independently-testable function rather than a
 * branchy shared one. It produces the same backend-agnostic CrackedResult the wpa-sec parser does, so the
 * one shared CrackedManifest stays unaware of which backend a crack came from (ADR-0109 #5).
 *
 * Fail-loud like the wpa-sec parser (quality bar §3): a line with fewer than four colons, an unparseable
 * BSSID field, or an empty/over-long essid or password is Malformed and yields no record, never a
 * half-filled one; the caller counts Malformed so a pwncrack format change surfaces as a non-zero count
 * rather than a silent parse-to-nothing. Being hardware-free and line-oriented it is unit-tested on the
 * native lane (ADR-0004 lane 1) and lets the device fetcher stream lines without buffering the account.
 */
#pragma once

#include <string_view>

#include "net/cracked_result.h"
#include "net/cracked_result_parser.h"  // ParseLineResult, the shared classification (backend-agnostic).

namespace sapper {

/// Parse one pwncrack potfile line into @p out. A trailing CR/LF (and trailing spaces, which the
/// endpoint has been seen to pad with) is tolerated. On Ok, @p out is fully populated; on Empty or
/// Malformed, @p out is left unchanged and must not be read. Reuses the wpa-sec parser's ParseLineResult
/// so the sync counts both sources' malformed/empty lines through one classification (ADR-0109 #5).
ParseLineResult parsePwncrackPotfileLine(std::string_view line, CrackedResult& out);

}  // namespace sapper
