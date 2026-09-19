/**
 * @file wpasec_response.h
 * @brief Pure, host-tested classification of wpa-sec's upload response (ADR-0017 decision #2).
 *
 * wpa-sec replies with hcxpcapngtool's verbose summary. The success signal is not the presence of a
 * label — hcx always prints "EAPOL pairs written to 22000 hash file...: N" and "PMKID(s) written to
 * 22000 hash file...: N" whether N is 0 or not — but the **count**. Classifying on the label alone
 * false-accepts a 0-result upload (fail-open: deleting a capture wpa-sec extracted nothing from). So
 * this parses the number after any "written to 22000 hash file" line and accepts only when a count is
 * >= 1; a duplicate is terminal success; everything else (including "No valid handshakes/PMKIDs
 * found" and all-zero counts) is Rejected — kept and retried, never assumed success (quality bar §3).
 *
 * Hardware-free so it is unit-tested against real captured wpa-sec responses on the native lane
 * (test/test_wpasec_response, ADR-0004 lane 1); the device wrapper (uploader_wpasec.cpp) only reads
 * the bytes off TLS and hands them here.
 */
#pragma once

#include "net/uploader.h"

namespace sapper {

/// Classify a complete wpa-sec response body (NUL-terminated). See the file comment for the rules.
UploadResult classifyWpaSecResponse(const char* response);

}  // namespace sapper
