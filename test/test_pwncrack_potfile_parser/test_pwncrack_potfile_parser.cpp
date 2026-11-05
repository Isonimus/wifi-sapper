/**
 * @file test_pwncrack_potfile_parser.cpp
 * @brief Native unit tests for the pure pwncrack potfile-line parser (ADR-0109 decision #5, §4 #46d).
 *
 * Pins the pwncrack field layout (BSSID index 1, ESSID index 3, password index 4) against wpa-sec's
 * (BSSID 0, essid 2, password after the third colon): the reason ADR-0109 #5 keeps a sibling parser
 * rather than one branchy shared one. The load-bearing cases are that the BSSID is read from the second
 * field (not the first), the ESSID from the fourth (not the first, which pwncrack uses for an auxiliary
 * name), and the password survives verbatim past the fourth colon. The fail-loud cases (quality bar §3)
 * prove a line with fewer than four colons, a bad BSSID, or an empty/over-long field yields no record
 * and is counted, never half-stored.
 */
#include <unity.h>

#include <cstring>
#include <string>
#include <string_view>

#include "net/pwncrack_potfile_parser.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

static void assertBssid(const uint8_t got[6], uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                        uint8_t b4, uint8_t b5) {
    const uint8_t want[6] = {b0, b1, b2, b3, b4, b5};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, 6);
}

void test_basic_line_reads_bssid_from_field1_and_essid_from_field3(void) {
    // field0 is an auxiliary name (ignored); field1 is the BSSID; field3 is the authoritative ESSID.
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(
                          "HomeAP:aabbccddeeff:extra:CoffeeShop:hunter2", r)));
    assertBssid(r.bssid, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
    TEST_ASSERT_EQUAL_STRING("CoffeeShop", r.essid);
    TEST_ASSERT_EQUAL_STRING("hunter2", r.password);
}

void test_essid_is_field3_not_field0(void) {
    // Guards the one hazard of the layout: field0 and field3 can both look like a name, and reading the
    // wrong one keys the crack to the wrong network. field3 wins.
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(
                          "WRONG-name-field0:112233445566:x:Right-essid:pw", r)));
    TEST_ASSERT_EQUAL_STRING("Right-essid", r.essid);
}

void test_password_with_colons_survives_verbatim(void) {
    // Everything after the fourth colon is the password, kept whole (the pwncrack analogue of the
    // wpa-sec colon-safe fix): generated PSKs often contain ':'.
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(
                          "name:aabbccddeeff:x:Net:p@ss:w0rd:!", r)));
    TEST_ASSERT_EQUAL_STRING("p@ss:w0rd:!", r.password);
    TEST_ASSERT_EQUAL_STRING("Net", r.essid);
}

void test_hyphen_grouped_bssid_is_accepted(void) {
    // The potfile has been seen to write the MAC either bare or hyphen-grouped; both resolve to the same
    // six octets (matches the sibling Adversary's parseBssid tolerance).
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(
                          "name:aa-bb-cc-dd-ee-ff:x:Net:pw", r)));
    assertBssid(r.bssid, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
}

void test_uppercase_hex_bssid_parses(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine("name:AABBCCDDEEFF:x:Net:pw", r)));
    assertBssid(r.bssid, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff);
}

void test_trailing_crlf_and_spaces_are_tolerated(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbccddeeff:x:Net:pw  \r\n", r)));
    TEST_ASSERT_EQUAL_STRING("pw", r.password);
}

void test_blank_line_is_empty_not_malformed(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Empty),
                      static_cast<int>(parsePwncrackPotfileLine("", r)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Empty),
                      static_cast<int>(parsePwncrackPotfileLine("   \r\n", r)));
}

void test_fewer_than_four_colons_is_malformed(void) {
    // Three colons is a valid wpa-sec line but an incomplete pwncrack one (no password field): Malformed,
    // so a wpa-sec-shaped body fed to this parser surfaces loud rather than mis-parsing.
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbccddeeff:x:Net", r)));
}

void test_unparseable_bssid_is_malformed(void) {
    CrackedResult r;
    // Wrong length in field1.
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbcc:x:Net:pw", r)));
    // Non-hex character in field1.
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbccddeegg:x:Net:pw", r)));
}

void test_empty_essid_or_password_is_malformed(void) {
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbccddeeff:x::pw", r)));
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("name:aabbccddeeff:x:Net:", r)));
}

void test_overlong_field_is_malformed_never_truncated(void) {
    std::string_view line =
        "name:aabbccddeeff:x:Net:"
        "0123456789012345678901234567890123456789012345678901234567890123456789";  // 70-char password
    CrackedResult r;
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine(line, r)));
}

void test_field_length_boundary_accepts_cap_minus_one_rejects_cap(void) {
    // The write-time guard is `size() >= kCrackedEssidCap`/`kCrackedPasswordCap`, load-bearing for buffer
    // safety: a field of exactly the cap would memcpy `cap` bytes then write out[cap] = '\0', a one-byte
    // overrun of the `char[cap]` member. Exercise the exact edge so a regression of `>=` to `>` (which
    // would accept a cap-length field and overflow) fails here; the 70-char test above cannot catch it.
    CrackedResult r;

    // Password: cap-1 (64) fits, cap (65) is rejected.
    const std::string pwFits = "name:aabbccddeeff:x:Net:" + std::string(kCrackedPasswordCap - 1, 'p');
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(pwFits, r)));
    TEST_ASSERT_EQUAL_size_t(kCrackedPasswordCap - 1, std::strlen(r.password));
    const std::string pwOver = "name:aabbccddeeff:x:Net:" + std::string(kCrackedPasswordCap, 'p');
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine(pwOver, r)));

    // ESSID: cap-1 (32) fits, cap (33) is rejected.
    const std::string essidFits = "name:aabbccddeeff:x:" + std::string(kCrackedEssidCap - 1, 'e') + ":pw";
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Ok),
                      static_cast<int>(parsePwncrackPotfileLine(essidFits, r)));
    TEST_ASSERT_EQUAL_size_t(kCrackedEssidCap - 1, std::strlen(r.essid));
    const std::string essidOver = "name:aabbccddeeff:x:" + std::string(kCrackedEssidCap, 'e') + ":pw";
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine(essidOver, r)));
}

void test_malformed_line_leaves_output_untouched(void) {
    CrackedResult r;
    std::strcpy(r.essid, "sentinel");
    std::strcpy(r.password, "keepme");
    TEST_ASSERT_EQUAL(static_cast<int>(ParseLineResult::Malformed),
                      static_cast<int>(parsePwncrackPotfileLine("garbage-no-colons", r)));
    TEST_ASSERT_EQUAL_STRING("sentinel", r.essid);
    TEST_ASSERT_EQUAL_STRING("keepme", r.password);
}

void test_a_mixed_body_yields_records_and_a_countable_malformed_tally(void) {
    static const char* kLines[] = {
        "name:aabbccddeeff:x:Alpha:pw1",      // Ok
        "not-a-line",                          // Malformed (no colons)
        "name:112233445566:x:Bravo:pw2",       // Ok
        "name:zzzz33445566:x:Bad:pw",          // Malformed (bad BSSID)
        "",                                    // Empty (not counted)
        "name:223344556677:x:Charlie:pw3",     // Ok
    };
    size_t okCount = 0;
    size_t malformedCount = 0;
    for (const char* line : kLines) {
        CrackedResult r;
        switch (parsePwncrackPotfileLine(line, r)) {
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
    RUN_TEST(test_basic_line_reads_bssid_from_field1_and_essid_from_field3);
    RUN_TEST(test_essid_is_field3_not_field0);
    RUN_TEST(test_password_with_colons_survives_verbatim);
    RUN_TEST(test_hyphen_grouped_bssid_is_accepted);
    RUN_TEST(test_uppercase_hex_bssid_parses);
    RUN_TEST(test_trailing_crlf_and_spaces_are_tolerated);
    RUN_TEST(test_blank_line_is_empty_not_malformed);
    RUN_TEST(test_fewer_than_four_colons_is_malformed);
    RUN_TEST(test_unparseable_bssid_is_malformed);
    RUN_TEST(test_empty_essid_or_password_is_malformed);
    RUN_TEST(test_overlong_field_is_malformed_never_truncated);
    RUN_TEST(test_field_length_boundary_accepts_cap_minus_one_rejects_cap);
    RUN_TEST(test_malformed_line_leaves_output_untouched);
    RUN_TEST(test_a_mixed_body_yields_records_and_a_countable_malformed_tally);
    return UNITY_END();
}
