/**
 * @file test_pwncrack_response.cpp
 * @brief Native unit tests for pwncrack upload-response classification (ADR-0109 #6 + amendment, §4 #46e).
 *
 * The four fixtures are frozen on the bodies the live probe measured on 2026-11-05 (artifacts/
 * pwncrack-response-probe.txt): accept, re-upload, and malformed-content all answer HTTP 200
 * `{"success":true}`, and a bad key answers HTTP 400 `{"error":"Invalid session key."}`. No fixture encodes
 * a guessed schema (quality bar §3). The fail-closed cases prove the classifier rejects a non-JSON 200 (a
 * Cloudflare HTML challenge), a wrong status, a false/mistyped/missing `success`, and an over-bound body, so
 * an unrecognised reply is never assumed success. No device needed (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>
#include <string>

#include "net/pwncrack_response.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

static UploadResult classify(int status, const char* body) {
    return classifyPwncrackResponse(status, body, std::strlen(body));
}

// --- The four measured legs (frozen fixtures). ---

// The three success legs (leg 1 accept, leg 2 re-upload, leg 3 malformed content) all answer HTTP 200
// {"success":true}: all Accepted, never Duplicate. pwncrack cannot distinguish a fresh submit from a
// re-submit (no dup signal), and content is validated async server-side so a malformed line still gets a
// success body here (harmless: the firmware only uploads well-formed WPA*01 lines, and pwncrack is the
// secondary backend under both-terminal removal, so a residual accept costs no handshake, ADR-0109 amendment).
void test_the_success_body_is_accepted(void) {
    TEST_ASSERT_EQUAL(UploadResult::Accepted, classify(200, "{\"success\":true}"));
}

// Leg 4 bad key answers HTTP 400 {"error":"Invalid session key."}: Rejected (synchronous auth fails loud).
void test_bad_key_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(400, "{\"error\":\"Invalid session key.\"}"));
}

// A JSON root that is not an object (a bare boolean, a number, an array, a string) is not the pwncrack
// shape and is Rejected, even a bare `true` that a laxer check might coerce (review finding, no-coercion).
void test_non_object_json_root_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "true"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "1"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "[true]"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "\"success\""));
}

// --- Fail-closed cases. ---

// A 200 whose body is a Cloudflare HTML interstitial is not JSON: Rejected (a substring search would not be).
void test_non_json_200_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected,
                      classify(200, "<!DOCTYPE html><html><head><title>Just a moment...</title></head></html>"));
}

// The positive JSON body with a non-200 status is Rejected: the status gate holds.
void test_success_body_with_non_200_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(500, "{\"success\":true}"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200 + 3, "{\"success\":true}"));  // e.g. 203, not 200.
}

// success:false, a string "true", a number 1, and a missing success key are each Rejected (strict boolean).
void test_success_false_or_mistyped_or_missing_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "{\"success\":false}"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "{\"success\":\"true\"}"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "{\"success\":1}"));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classify(200, "{\"error\":\"something\"}"));
}

// An empty body, a nullptr, and a body over the bound are each Rejected before any parse.
void test_empty_null_and_overbound_rejected(void) {
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classifyPwncrackResponse(200, "", 0));
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classifyPwncrackResponse(200, nullptr, 16));
    // A valid-prefixed but over-long body: rejected on the bound, not parsed.
    std::string big = "{\"success\":true,\"pad\":\"";
    big.append(kPwncrackResponseMax, 'x');
    big += "\"}";
    TEST_ASSERT_TRUE(big.size() > kPwncrackResponseMax);
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classifyPwncrackResponse(200, big.data(), big.size()));
}

// The over-bound guard is reachable at the exact boundary: a valid success object of exactly
// kPwncrackResponseMax bytes parses and is Accepted (it read complete), while one of kPwncrackResponseMax+1
// bytes is Rejected before any parse (the device reads into a +1 buffer so a filled reply is detectably
// over-bound, review MED #1). Pins both sides of the bound.
void test_body_bound_boundary(void) {
    const std::string base = "{\"success\":true,\"p\":\"";  // + pad + "\"}" closes the object.
    const size_t overhead = base.size() + 2;               // the trailing `"}`.
    std::string at = base + std::string(kPwncrackResponseMax - overhead, 'x') + "\"}";
    TEST_ASSERT_EQUAL_size_t(kPwncrackResponseMax, at.size());
    TEST_ASSERT_EQUAL(UploadResult::Accepted, classifyPwncrackResponse(200, at.data(), at.size()));

    std::string over = base + std::string(kPwncrackResponseMax + 1 - overhead, 'x') + "\"}";
    TEST_ASSERT_EQUAL_size_t(kPwncrackResponseMax + 1, over.size());
    TEST_ASSERT_EQUAL(UploadResult::Rejected, classifyPwncrackResponse(200, over.data(), over.size()));
}

// The classifier reads exactly `len` bytes, not a NUL terminator: a success object followed by trailing
// non-JSON bytes beyond `len` must not be read, and a truncated-at-len success object still classifies on
// what was given. (Guards against a future switch to strlen that would overread an unterminated TLS buffer.)
void test_respects_explicit_length(void) {
    const char buf[] = "{\"success\":true}GARBAGE_PAST_THE_JSON";
    const size_t jsonLen = std::strlen("{\"success\":true}");
    TEST_ASSERT_EQUAL(UploadResult::Accepted, classifyPwncrackResponse(200, buf, jsonLen));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_the_success_body_is_accepted);
    RUN_TEST(test_bad_key_rejected);
    RUN_TEST(test_non_object_json_root_rejected);
    RUN_TEST(test_non_json_200_rejected);
    RUN_TEST(test_success_body_with_non_200_rejected);
    RUN_TEST(test_success_false_or_mistyped_or_missing_rejected);
    RUN_TEST(test_empty_null_and_overbound_rejected);
    RUN_TEST(test_body_bound_boundary);
    RUN_TEST(test_respects_explicit_length);
    return UNITY_END();
}
