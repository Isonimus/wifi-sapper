/**
 * @file pwncrack_response.cpp
 * @brief Implementation of pwncrack upload-response classification (ADR-0109). See pwncrack_response.h.
 */
#include "net/pwncrack_response.h"

#include <ArduinoJson.h>

namespace sapper {

UploadResult classifyPwncrackResponse(int httpStatus, const char* body, size_t len) {
    // Only an HTTP 200 can be a success. The bad-key case answers HTTP 400 (synchronous auth, measured
    // 2026-11-05), so gating on the status here makes a misprovisioned key fail loud as a reject that keeps
    // and retries, never a silent submitted-mark (ADR-0109 amendment).
    if (httpStatus != 200) return UploadResult::Rejected;

    // Bound the body before touching the parser: an over-long or absent body is Rejected, so nothing is
    // ever allocated proportional to the reply (the ADR-0076 no-PSRAM-TLS discipline, ADR-0109 #6).
    if (body == nullptr || len == 0 || len > kPwncrackResponseMax) return UploadResult::Rejected;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body, len);
    if (err) return UploadResult::Rejected;  // not JSON (e.g. a Cloudflare HTML challenge served 200): reject.

    // The contract's body is a JSON *object* (`{"success": true}` / `{"error": ...}`). Require an object
    // root explicitly: a non-object root (a bare `true`, a number, an array) is not the pwncrack shape, so
    // reject it rather than lean on the key lookup below happening to return null for a non-object.
    if (!doc.is<JsonObject>()) return UploadResult::Rejected;

    // `success` must be a real JSON boolean true. Requiring the boolean type rejects a string "true" or a
    // numeric 1, so a truthy-but-wrong-typed field never false-accepts, and a missing key (the HTTP 400
    // `{"error": ...}` shape, or any unrecognised object) reads as not-a-bool and is Rejected.
    JsonVariantConst success = doc["success"];
    if (success.is<bool>() && success.as<bool>()) return UploadResult::Accepted;
    return UploadResult::Rejected;
}

}  // namespace sapper
