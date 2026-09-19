/**
 * @file test_wpasec_response.cpp
 * @brief Native unit tests for the wpa-sec response classifier (ADR-0017 decision #2).
 *
 * Fixtures are real wpa-sec/hcxpcapngtool 6.3.5 responses captured on hardware (slice-0018 on-air
 * verify). The central case is the one a label-only match got wrong: hcx always prints the
 * "written to 22000 hash file" line, so success must be read from its *count*, not its presence.
 */
#include <unity.h>

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

// wpa-sec already holds this capture — terminal success.
static const char* kDuplicate =
    "This handshake is already in database.\nEAPOL pairs written to 22000 hash file...: 0\n";

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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_positive_eapol_count_is_accepted);
    RUN_TEST(test_zero_count_is_rejected_not_accepted);
    RUN_TEST(test_positive_pmkid_count_is_accepted);
    RUN_TEST(test_no_handshakes_message_is_rejected);
    RUN_TEST(test_already_in_database_is_duplicate);
    RUN_TEST(test_empty_and_null_are_rejected);
    return UNITY_END();
}
