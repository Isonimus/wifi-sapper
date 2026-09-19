/**
 * @file test_cracked_parser.cpp
 * @brief Native unit tests for the pure wpa-sec cracked-line parser (ADR-0019 decision #3; slice-0020
 *        Scenarios A, B).
 *
 * The two cases that justify replacing the reference's strtok_r parse: a password containing ':'
 * survives verbatim (Scenario A), and a malformed line yields no record and is counted rather than
 * silently dropped or half-stored (Scenario B, quality bar §3).
 */
#include <unity.h>

#include <cstring>
#include <string_view>

#include "net/cracked_result_parser.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

static void assertBssid(const uint8_t got[6], uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                        uint8_t b4, uint8_t b5) {
    const uint8_t want[6] = {b0, b1, b2, b3, b4, b5};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, 6);
}

void test_basic_line_parses_all_fields(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parseCrackedLine("aabbccddeeff:1122334455ee:CoffeeShop:hunter2",
                                                        r)));
    assertBssid(r.bssid, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
    TEST_ASSERT_EQUAL_STRING("CoffeeShop", r.essid);
    TEST_ASSERT_EQUAL_STRING("hunter2", r.password);
}

void test_password_with_colons_survives_verbatim(void) {
    // Scenario A: the reference's strtok_r truncated this at the first inner colon; we keep it whole.
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parseCrackedLine(
                          "aabbccddeeff:1122334455ee:CoffeeShop:p@ss:w0rd:!", r)));
    TEST_ASSERT_EQUAL_STRING("p@ss:w0rd:!", r.password);
    TEST_ASSERT_EQUAL_STRING("CoffeeShop", r.essid);
}

void test_uppercase_hex_bssid_parses(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parseCrackedLine("AABBCCDDEEFF:001122334455:Net:pw", r)));
    assertBssid(r.bssid, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
}

void test_trailing_crlf_is_tolerated(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parseCrackedLine("aabbccddeeff:001122334455:Net:pw\r\n", r)));
    TEST_ASSERT_EQUAL_STRING("pw", r.password);
}

void test_blank_line_is_empty_not_malformed(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Empty),
                      static_cast<int>(parseCrackedLine("", r)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Empty),
                      static_cast<int>(parseCrackedLine("\r\n", r)));
}

void test_fewer_than_three_colons_is_malformed(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("aabbccddeeff:001122334455:NoPassword", r)));
}

void test_unparseable_ap_bssid_is_malformed(void) {
    CrackedResult r;
    // Wrong length.
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("aabbcc:001122334455:Net:pw", r)));
    // Non-hex character.
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("aabbccddeegg:001122334455:Net:pw", r)));
}

void test_empty_essid_or_password_is_malformed(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("aabbccddeeff:001122334455::pw", r)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("aabbccddeeff:001122334455:Net:", r)));
}

void test_overlong_field_is_malformed_never_truncated(void) {
    // A password longer than the buffer is skipped, not silently cut (quality bar §3).
    std::string_view line =
        "aabbccddeeff:001122334455:Net:"
        "0123456789012345678901234567890123456789012345678901234567890123456789";  // 70 chars
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine(line, r)));
}

void test_malformed_line_leaves_output_untouched(void) {
    CrackedResult r;
    std::strcpy(r.essid, "sentinel");
    std::strcpy(r.password, "keepme");
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parseCrackedLine("garbage-no-colons", r)));
    // No partial write: the record still holds the caller's prior contents.
    TEST_ASSERT_EQUAL_STRING("sentinel", r.essid);
    TEST_ASSERT_EQUAL_STRING("keepme", r.password);
}

void test_a_mixed_body_yields_records_and_a_countable_malformed_tally(void) {
    // Scenario B: valid lines around malformed ones still parse, and a caller can count the malformed
    // ones from the classification alone — the mechanism the sync's malformed count is built on.
    static const char* kLines[] = {
        "aabbccddeeff:001122334455:Alpha:pw1",  // Ok
        "not-a-line",                           // Malformed (no colons)
        "112233445566:001122334455:Bravo:pw2",  // Ok
        "zzzz33445566:001122334455:Bad:pw",     // Malformed (bad BSSID)
        "",                                     // Empty (not counted)
        "223344556677:001122334455:Charlie:pw3",  // Ok
    };
    size_t okCount = 0;
    size_t malformedCount = 0;
    for (const char* line : kLines) {
        CrackedResult r;
        switch (parseCrackedLine(line, r)) {
            case ParseLineResult::Ok: ++okCount; break;
            case ParseLineResult::Malformed: ++malformedCount; break;
            case ParseLineResult::Empty: break;
        }
    }
    TEST_ASSERT_EQUAL_size_t(3, okCount);
    TEST_ASSERT_EQUAL_size_t(2, malformedCount);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_basic_line_parses_all_fields);
    RUN_TEST(test_password_with_colons_survives_verbatim);
    RUN_TEST(test_uppercase_hex_bssid_parses);
    RUN_TEST(test_trailing_crlf_is_tolerated);
    RUN_TEST(test_blank_line_is_empty_not_malformed);
    RUN_TEST(test_fewer_than_three_colons_is_malformed);
    RUN_TEST(test_unparseable_ap_bssid_is_malformed);
    RUN_TEST(test_empty_essid_or_password_is_malformed);
    RUN_TEST(test_overlong_field_is_malformed_never_truncated);
    RUN_TEST(test_malformed_line_leaves_output_untouched);
    RUN_TEST(test_a_mixed_body_yields_records_and_a_countable_malformed_tally);
    return UNITY_END();
}
