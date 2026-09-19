/**
 * @file cracked_result_parser.h
 * @brief Pure parser for one wpa-sec `?api&dl=1` line into a CrackedResult (ADR-0019 decision #3).
 *
 * The download body is the account's whole cracked set, one result per line:
 *   `ap_bssid:client_bssid:essid:password`
 * where each BSSID is 12 hex nibbles. This unit turns one line into a CrackedResult, and its whole
 * reason to exist is the two fixes ADR-0019 records against the reference (adversary:wpasec_service):
 *
 *   - **Colon-safe password (force 3a).** The reference splits with strtok_r on ':', so a password
 *     containing ':' (common in generated PSKs) is silently truncated at the first inner colon. Here
 *     the line is split on the **first three** colons only; everything after the third is the password,
 *     verbatim. The two fixed-width BSSID fields make that boundary unambiguous.
 *   - **Fail loud, never partial (quality bar §3).** A line with fewer than three colons, an
 *     unparseable AP BSSID, or an empty/over-long essid or password is classified Malformed and yields
 *     no record — never a half-filled one. The caller counts Malformed, so a wpa-sec format change
 *     surfaces as a non-zero count rather than a silent parse-to-nothing.
 *
 * Being hardware-free and line-oriented, it is unit-tested on the native lane (ADR-0004 lane 1) and
 * lets the device fetcher stream lines off the HTTPClient reader without buffering the whole account.
 */
#pragma once

#include <string_view>

#include "net/cracked_result.h"

namespace sapper {

/// How one line parsed. Empty is a blank/whitespace-only line (ignored, not counted as a fault);
/// Malformed is a line that should have been a result but was not (counted, so a format change is loud).
enum class ParseLineResult { Ok, Empty, Malformed };

/// Parse one download line into @p out. A trailing '\r' (HTTP CRLF) is tolerated. On Ok, @p out is
/// fully populated; on Empty or Malformed, @p out is left unchanged and must not be read.
ParseLineResult parseCrackedLine(std::string_view line, CrackedResult& out);

}  // namespace sapper
