/**
 * @file wpasec_response.cpp
 * @brief Implementation of the pure wpa-sec response classifier (ADR-0017 decision #2).
 */
#include "net/wpasec_response.h"

#include <cstdlib>
#include <cstring>

namespace sapper {
namespace {

// hcx prints both a PMKID and an EAPOL "written to 22000 hash file" line; matching this shared tail
// and reading the count after it covers both without depending on the exact prefix or the "(s)".
constexpr const char* kWrittenLabel = "written to 22000 hash file";

/// From @p from, scan within the current line to the first digit and return its value; -1 if the line
/// holds no digit before its end. Bounds the search to the line so a count is never read from a later
/// one.
long countOnLine(const char* from) {
    const char* p = from;
    while (*p != '\0' && *p != '\n' && (*p < '0' || *p > '9')) ++p;
    if (*p < '0' || *p > '9') return -1;
    return std::strtol(p, nullptr, 10);
}

}  // namespace

UploadResult classifyWpaSecResponse(const char* response) {
    if (response == nullptr) return UploadResult::Rejected;

    // wpa-sec already holds this capture — terminal success, off our hands (ADR-0017 decision #2).
    if (std::strstr(response, "already in database") != nullptr) return UploadResult::Duplicate;

    // Accept only on a positive written count. Every "written to 22000 hash file" line is checked, so
    // a capture that produced a PMKID *or* an EAPOL pair counts; a run of ": 0" lines does not.
    for (const char* at = std::strstr(response, kWrittenLabel); at != nullptr;
         at = std::strstr(at + 1, kWrittenLabel)) {
        if (countOnLine(at + std::strlen(kWrittenLabel)) >= 1) return UploadResult::Accepted;
    }

    // No positive count: "No valid handshakes/PMKIDs found", all-zero summaries, a truncated read, or
    // an unrecognised body. Fail closed — keep the capture and retry, never assume success.
    return UploadResult::Rejected;
}

}  // namespace sapper
