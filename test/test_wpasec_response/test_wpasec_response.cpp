/**
 * @file test_wpasec_response.cpp
 * @brief Native unit tests for the wpa-sec response classifier (ADR-0017 decision #2, ADR-0076).
 *
 * Fixtures are real wpa-sec/hcxpcapngtool 6.3.5 responses captured on hardware (slice-0018 on-air
 * verify). The central case is the one a label-only match got wrong: hcx always prints the
 * "written to 22000 hash file" line, so success must be read from its *count*, not its presence.
 *
 * ADR-0076 made the classifier streaming (the former whole-reply 16KB buffer failed on the no-PSRAM S3).
 * So beyond the verdict-per-fixture cases, every fixture is also classified fed in every chunk-boundary
 * split: a verdict must not depend on how TLS happened to split the bytes (a token or a count digit run
 * straddling a chunk is the hazard streaming introduces).
 */
#include <unity.h>

#include <cstring>

#include "net/wpasec_response.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

// A real accepted response (<redacted-ssid>): one EAPOL pair written. Trimmed to the relevant lines.
static const char* kAcceptedEapol =
    "summary capture file\n"
    "--------------------\n"
    "EAPOL pairs (total)......................: 1\n"
    "EAPOL pairs written to 22000 hash file...: 1 (RC checked)\n"
    "EAPOL M12E2 (challenge - ANONCE from M1).: 1\n"
    "\nsession summary\n---------------\nprocessed cap files...................: 1\n";

// Same shape but hcx extracted nothing — the always-present label carries a zero count. This is the
// case a label-only classifier false-accepted (fail-open); it must be Rejected.
static const char* kZeroCount =
    "summary capture file\n"
    "EAPOL pairs (total)......................: 0\n"
    "EAPOL pairs written to 22000 hash file...: 0\n"
    "\nInformation: missing frames!\nThis dump file does not contain enough EAPOL M1 frames.\n";

// A PMKID success uses a different prefix ("PMKID(s)") but the same "written to 22000 hash file" tail.
static const char* kAcceptedPmkid =
    "PMKID(s)................................: 1\n"
    "PMKID(s) written to 22000 hash file......: 1\n"
    "EAPOL pairs written to 22000 hash file...: 0\n";

// wpa-sec's short message when the file yields nothing hcx can parse at all (our synthetic stimulus).
static const char* kNoHandshakes = "No valid handshakes/PMKIDs found in the submitted file.\n";

// wpa-sec already holds this capture — terminal success. The ": 0" line is irrelevant.
static const char* kDuplicate =
    "This handshake is already in database.\nEAPOL pairs written to 22000 hash file...: 0\n";

// Duplicate AND a *positive* written count in one body, in both orders. Duplicate must still win (ADR-0017
// #2 / ADR-0076 #2): wpa-sec can return the hcx summary and the dedup line together.
static const char* kDuplicateBeforePositive =
    "This handshake is already in database.\nEAPOL pairs written to 22000 hash file...: 1\n";
static const char* kPositiveBeforeDuplicate =
    "EAPOL pairs written to 22000 hash file...: 1\nThis handshake is already in database.\n";

// A genuine duplicate phrase preceded by a prefix collision: "alre" then the real "already in database".
// A naive first-char-restart matcher matches "alrea", fails on the next char, resets to 0, and never
// re-detects the real occurrence at offset 4 — classifying this Rejected instead of Duplicate. The KMP
// matcher falls back correctly and finds it. Regression for the slice-0077 adversarial finding.
static const char* kDuplicateAfterPrefixCollision = "alrealready in database\n";

/// Feed @p resp through the streaming classifier in fixed-size @p chunk slices, returning the verdict.
static UploadResult classifyInChunks(const char* resp, size_t chunk) {
    WpaSecResponseClassifier classifier;
    const size_t len = std::strlen(resp);
    for (size_t i = 0; i < len; i += chunk) {
        const size_t n = (i + chunk <= len) ? chunk : (len - i);
        classifier.feed(resp + i, n);
    }
    return classifier.result();
}

// ---- verdict per fixture (the whole-string wrapper) ------------------------------------------------

void test_positive_eapol_count_is_accepted(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Accepted),
                      static_cast<int>(classifyWpaSecResponse(kAcceptedEapol)));
}

void test_zero_count_is_rejected_not_accepted(void) {
    // The regression: the label is present but the count is 0 — must not be read as success.
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Rejected),
                      static_cast<int>(classifyWpaSecResponse(kZeroCount)));
}

void test_positive_pmkid_count_is_accepted(void) {
    // EAPOL count 0 but PMKID count 1 — any positive written count is a success.
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Accepted),
                      static_cast<int>(classifyWpaSecResponse(kAcceptedPmkid)));
}

void test_no_handshakes_message_is_rejected(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Rejected),
                      static_cast<int>(classifyWpaSecResponse(kNoHandshakes)));
}

void test_already_in_database_is_duplicate(void) {
    // Duplicate outranks the ": 0" written line in the same body.
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Duplicate),
                      static_cast<int>(classifyWpaSecResponse(kDuplicate)));
}

void test_empty_and_null_are_rejected(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Rejected),
                      static_cast<int>(classifyWpaSecResponse("")));
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Rejected),
                      static_cast<int>(classifyWpaSecResponse(nullptr)));
}

// ---- duplicate outranks a positive count, in any order (ADR-0076 #2) -------------------------------

void test_duplicate_outranks_positive_count_either_order(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Duplicate),
                      static_cast<int>(classifyWpaSecResponse(kDuplicateBeforePositive)));
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Duplicate),
                      static_cast<int>(classifyWpaSecResponse(kPositiveBeforeDuplicate)));
}

void test_duplicate_detected_despite_prefix_collision(void) {
    // Must find the real "already in database" at offset 4, not be thrown off by the leading "alre" that
    // collides with the token's own prefix (a naive restart misses it; KMP does not).
    TEST_ASSERT_EQUAL(static_cast<int>(UploadResult::Duplicate),
                      static_cast<int>(classifyWpaSecResponse(kDuplicateAfterPrefixCollision)));
}

// ---- streaming is split-invariant (ADR-0076): every fixture, every chunk size -----------------------

void test_streaming_verdict_is_split_invariant(void) {
    struct Case {
        const char* resp;
        UploadResult expected;
    };
    const Case cases[] = {
        {kAcceptedEapol, UploadResult::Accepted},
        {kZeroCount, UploadResult::Rejected},
        {kAcceptedPmkid, UploadResult::Accepted},
        {kNoHandshakes, UploadResult::Rejected},
        {kDuplicate, UploadResult::Duplicate},
        {kDuplicateBeforePositive, UploadResult::Duplicate},
        {kPositiveBeforeDuplicate, UploadResult::Duplicate},
        {kDuplicateAfterPrefixCollision, UploadResult::Duplicate},
    };
    // Chunk sizes chosen to split tokens and count digit runs at many different offsets; 1 is the maximal
    // split (every byte its own feed), which stresses a token/count straddling a chunk the hardest.
    const size_t chunkSizes[] = {1, 2, 3, 5, 7, 13, 64, 4096};
    for (const Case& c : cases) {
        for (size_t chunk : chunkSizes) {
            TEST_ASSERT_EQUAL(static_cast<int>(c.expected),
                              static_cast<int>(classifyInChunks(c.resp, chunk)));
        }
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_positive_eapol_count_is_accepted);
    RUN_TEST(test_zero_count_is_rejected_not_accepted);
    RUN_TEST(test_positive_pmkid_count_is_accepted);
    RUN_TEST(test_no_handshakes_message_is_rejected);
    RUN_TEST(test_already_in_database_is_duplicate);
    RUN_TEST(test_empty_and_null_are_rejected);
    RUN_TEST(test_duplicate_outranks_positive_count_either_order);
    RUN_TEST(test_duplicate_detected_despite_prefix_collision);
    RUN_TEST(test_streaming_verdict_is_split_invariant);
    return UNITY_END();
}
