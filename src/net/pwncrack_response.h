/**
 * @file pwncrack_response.h
 * @brief Pure, host-tested classification of pwncrack's upload response (ADR-0109 decision #6 + amendment).
 *
 * The pwncrack analogue of wpa-sec's WpaSecResponseClassifier (ADR-0076), fail-closed exactly the same way:
 * a capture is marked submitted only on a *positive* Accepted; everything else is Rejected, kept and
 * retried, never assumed uploaded (quality bar §3). It differs from the wpa-sec classifier in three ways
 * the live probe pinned (2026-11-05, ADR-0109 amendment, artifacts/pwncrack-response-probe.txt):
 *
 * - The reply is **JSON**, not hcx's verbose text, so this parses JSON (not substrings): a Cloudflare HTML
 *   challenge served with HTTP 200 does not parse and is Rejected, where a substring search for "success"
 *   could false-accept it.
 * - The contract depends on **both** the HTTP status and the body: Accepted only on HTTP 200 with a JSON
 *   body whose `success` is boolean true; a bad key answers HTTP 400 `{"error": "..."}` and is Rejected
 *   (synchronous auth, so a misprovisioned key fails loud, never a silent submitted-mark).
 * - pwncrack has **no duplicate signal** (a fresh submit and a re-submit both answer `{"success": true}`),
 *   so there is no Duplicate branch: the verdict is Accepted or Rejected.
 *
 * The body is tiny (16-32 bytes measured), but the parse is **bounded**: a body over kPwncrackResponseMax
 * is Rejected before any parse, so no allocation is ever sized to the reply (the no-PSRAM ESP32-S3 TLS
 * discipline that drove ADR-0076 applies identically). Hardware-free: unit-tested on the native lane against
 * the four measured response bodies (test/test_pwncrack_response, ADR-0004 lane 1).
 */
#pragma once

#include <cstddef>

#include "net/uploader.h"  // reuse UploadResult.

namespace sapper {

/// Largest upload-response body this classifier parses. The measured bodies are 16-32 bytes; a body over
/// this bound is Rejected before any parse, so no heap is ever sized to the reply (ADR-0109 #6 / ADR-0076).
constexpr size_t kPwncrackResponseMax = 256;

/**
 * @brief Classify one pwncrack upload response.
 *
 * @param httpStatus the HTTP status line code (e.g. 200, 400).
 * @param body       the response body; need not be NUL-terminated (parsed over @p len).
 * @param len        length of @p body in bytes.
 * @return Accepted only when @p httpStatus is 200 and @p body is JSON whose `success` is boolean true;
 *         Rejected on everything else (non-200, nullptr/empty/over-bound body, non-JSON, a `success` that
 *         is missing, false, or not a boolean). Never Duplicate (pwncrack has no duplicate signal).
 */
UploadResult classifyPwncrackResponse(int httpStatus, const char* body, size_t len);

}  // namespace sapper
