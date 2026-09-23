/**
 * @file test_rssi_histogram.cpp
 * @brief Native unit tests for the pure RSSI↔capture accumulator (ADR-0067).
 *
 * The untethered probe's data is only as trustworthy as this accumulator, which nobody watches during a
 * multi-hour run. Covers: band assignment across every edge and the open extremes (the ADR-0067 review's
 * `rssi >= 0` mis-bin), attempt recording into bands and the na bucket, resume-vs-reset by run tag, and
 * the serialize/deserialize round-trip plus its fail-loud rejects. No filesystem, no radio.
 */
#include <unity.h>

#include <cstring>

#include "net/rssi_histogram.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

void test_band_edges_are_lower_inclusive_and_open_at_extremes(void) {
    // Band 0 (>= -50) is open at the top: a 0-or-positive reading is very strong, never very weak.
    TEST_ASSERT_EQUAL_UINT(0, rssiBand(10));
    TEST_ASSERT_EQUAL_UINT(0, rssiBand(0));
    TEST_ASSERT_EQUAL_UINT(0, rssiBand(-50));  // edge is lower-inclusive to the stronger band.
    TEST_ASSERT_EQUAL_UINT(1, rssiBand(-51));
    TEST_ASSERT_EQUAL_UINT(1, rssiBand(-60));
    TEST_ASSERT_EQUAL_UINT(2, rssiBand(-61));
    TEST_ASSERT_EQUAL_UINT(2, rssiBand(-70));
    TEST_ASSERT_EQUAL_UINT(3, rssiBand(-71));
    TEST_ASSERT_EQUAL_UINT(3, rssiBand(-80));
    TEST_ASSERT_EQUAL_UINT(4, rssiBand(-81));
    TEST_ASSERT_EQUAL_UINT(4, rssiBand(-128));  // band 4 is open at the bottom.
}

void test_records_attempts_into_bands_and_na_bucket(void) {
    RssiHistogram h;
    recordAttempt(h, -45, true);    // band 0, captured
    recordAttempt(h, -45, false);   // band 0, empty
    recordAttempt(h, -75, true);    // band 3, captured
    recordAttempt(h, kRssiUnknown, true);   // na, captured
    recordAttempt(h, kRssiUnknown, false);  // na, empty

    TEST_ASSERT_EQUAL_UINT32(2, h.attempts[0]);
    TEST_ASSERT_EQUAL_UINT32(1, h.captured[0]);
    TEST_ASSERT_EQUAL_UINT32(1, h.attempts[3]);
    TEST_ASSERT_EQUAL_UINT32(1, h.captured[3]);
    TEST_ASSERT_EQUAL_UINT32(0, h.attempts[1]);
    TEST_ASSERT_EQUAL_UINT32(2, h.naAttempts);
    TEST_ASSERT_EQUAL_UINT32(1, h.naCaptured);
}

void test_reset_for_tag_zeroes_counters_and_adopts_tag(void) {
    RssiHistogram h;
    recordAttempt(h, -55, true);
    resetForTag(h, "2026-09-23-lab");
    TEST_ASSERT_EQUAL_STRING("2026-09-23-lab", h.runTag);
    TEST_ASSERT_EQUAL_UINT32(0, h.attempts[1]);
    TEST_ASSERT_EQUAL_UINT32(0, h.captured[1]);
    TEST_ASSERT_EQUAL_UINT32(0, h.naAttempts);
}

void test_reset_for_tag_truncates_an_overlong_tag(void) {
    RssiHistogram h;
    resetForTag(h, "this-run-tag-is-far-too-long-to-fit-in-the-field");
    TEST_ASSERT_EQUAL_UINT(kRssiRunTagLen - 1, std::strlen(h.runTag));  // truncated, still terminated.
}

void test_round_trips_through_serialization(void) {
    RssiHistogram h;
    resetForTag(h, "run-A");
    recordAttempt(h, -40, true);
    recordAttempt(h, -65, false);
    recordAttempt(h, -90, true);
    recordAttempt(h, kRssiUnknown, true);

    uint8_t buf[kRssiHistogramBytes];
    TEST_ASSERT_EQUAL_UINT(kRssiHistogramBytes, serializeHistogram(h, buf, sizeof(buf)));

    RssiHistogram back;
    TEST_ASSERT_TRUE(deserializeHistogram(buf, sizeof(buf), back));
    TEST_ASSERT_EQUAL_STRING("run-A", back.runTag);
    TEST_ASSERT_EQUAL_UINT32(1, back.captured[0]);
    TEST_ASSERT_EQUAL_UINT32(1, back.attempts[2]);   // -65 → band 2, empty
    TEST_ASSERT_EQUAL_UINT32(0, back.captured[2]);
    TEST_ASSERT_EQUAL_UINT32(1, back.captured[4]);   // -90 → band 4, captured
    TEST_ASSERT_EQUAL_UINT32(1, back.naCaptured);
}

void test_serialize_fails_loud_on_a_short_buffer(void) {
    RssiHistogram h;
    uint8_t buf[kRssiHistogramBytes - 1];
    TEST_ASSERT_EQUAL_UINT(0, serializeHistogram(h, buf, sizeof(buf)));  // never a truncated record.
}

void test_run_tag_fits_boundary_and_rejects_overlong(void) {
    TEST_ASSERT_FALSE(runTagFits(nullptr));
    char maxTag[kRssiRunTagLen];  // kRssiRunTagLen-1 chars + terminator = fits exactly.
    std::memset(maxTag, 'a', kRssiRunTagLen - 1);
    maxTag[kRssiRunTagLen - 1] = '\0';
    TEST_ASSERT_TRUE(runTagFits(maxTag));
    char overTag[kRssiRunTagLen + 1];  // kRssiRunTagLen chars = one too long, no room for terminator.
    std::memset(overTag, 'a', kRssiRunTagLen);
    overTag[kRssiRunTagLen] = '\0';
    TEST_ASSERT_FALSE(runTagFits(overTag));
}

void test_histogram_has_tag_is_exact_without_truncation_collision(void) {
    RssiHistogram h;
    resetForTag(h, "run-A");
    TEST_ASSERT_TRUE(histogramHasTag(h, "run-A"));
    TEST_ASSERT_FALSE(histogramHasTag(h, "run-B"));
    TEST_ASSERT_FALSE(histogramHasTag(h, nullptr));

    // The pre-fix bug: two tags identical in their first 23 chars but differing at the 24th would both
    // truncate to the same stored form and resume into one histogram. With full-field comparison they are
    // distinct. (23 'a' + differing 24th char, both well within the 48-byte field.)
    char tagX[25];
    char tagY[25];
    std::memset(tagX, 'a', 23);
    std::memset(tagY, 'a', 23);
    tagX[23] = 'X';
    tagX[24] = '\0';
    tagY[23] = 'Y';
    tagY[24] = '\0';
    RssiHistogram hx;
    resetForTag(hx, tagX);
    TEST_ASSERT_TRUE(runTagFits(tagX));
    TEST_ASSERT_TRUE(histogramHasTag(hx, tagX));
    TEST_ASSERT_FALSE(histogramHasTag(hx, tagY));
}

void test_deserialize_terminates_an_unterminated_stored_tag(void) {
    RssiHistogram h;
    resetForTag(h, "run");
    uint8_t buf[kRssiHistogramBytes];
    serializeHistogram(h, buf, sizeof(buf));
    // A foreign/corrupt writer that filled all tag bytes with no terminator (tag starts after 4 magic + 1
    // version). Deserialize must not let an over-long/garbage string propagate.
    std::memset(buf + 5, 'Z', kRssiRunTagLen);
    RssiHistogram out;
    TEST_ASSERT_TRUE(deserializeHistogram(buf, sizeof(buf), out));
    TEST_ASSERT_EQUAL_UINT(kRssiRunTagLen - 1, std::strlen(out.runTag));
}

void test_round_trip_covers_every_band(void) {
    RssiHistogram h;
    resetForTag(h, "all-bands");
    recordAttempt(h, -40, true);            // band 0 captured
    recordAttempt(h, -55, false);           // band 1 empty
    recordAttempt(h, -65, true);            // band 2 captured
    recordAttempt(h, -75, false);           // band 3 empty
    recordAttempt(h, -95, true);            // band 4 captured
    recordAttempt(h, kRssiUnknown, false);  // na empty

    uint8_t buf[kRssiHistogramBytes];
    serializeHistogram(h, buf, sizeof(buf));
    RssiHistogram back;
    TEST_ASSERT_TRUE(deserializeHistogram(buf, sizeof(buf), back));
    TEST_ASSERT_EQUAL_UINT32(1, back.captured[0]);
    TEST_ASSERT_EQUAL_UINT32(1, back.attempts[1]);
    TEST_ASSERT_EQUAL_UINT32(0, back.captured[1]);
    TEST_ASSERT_EQUAL_UINT32(1, back.captured[2]);
    TEST_ASSERT_EQUAL_UINT32(1, back.attempts[3]);
    TEST_ASSERT_EQUAL_UINT32(0, back.captured[3]);
    TEST_ASSERT_EQUAL_UINT32(1, back.captured[4]);
    TEST_ASSERT_EQUAL_UINT32(1, back.naAttempts);
    TEST_ASSERT_EQUAL_UINT32(0, back.naCaptured);
}

void test_deserialize_rejects_bad_magic_version_and_length(void) {
    RssiHistogram h;
    resetForTag(h, "run-B");
    recordAttempt(h, -40, true);
    uint8_t buf[kRssiHistogramBytes];
    serializeHistogram(h, buf, sizeof(buf));

    RssiHistogram out;
    TEST_ASSERT_FALSE(deserializeHistogram(buf, kRssiHistogramBytes - 1, out));  // too short.

    uint8_t badMagic[kRssiHistogramBytes];
    std::memcpy(badMagic, buf, sizeof(badMagic));
    badMagic[0] = 'X';
    TEST_ASSERT_FALSE(deserializeHistogram(badMagic, sizeof(badMagic), out));

    uint8_t badVersion[kRssiHistogramBytes];
    std::memcpy(badVersion, buf, sizeof(badVersion));
    badVersion[4] = 0xFF;
    TEST_ASSERT_FALSE(deserializeHistogram(badVersion, sizeof(badVersion), out));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_band_edges_are_lower_inclusive_and_open_at_extremes);
    RUN_TEST(test_records_attempts_into_bands_and_na_bucket);
    RUN_TEST(test_reset_for_tag_zeroes_counters_and_adopts_tag);
    RUN_TEST(test_reset_for_tag_truncates_an_overlong_tag);
    RUN_TEST(test_round_trips_through_serialization);
    RUN_TEST(test_run_tag_fits_boundary_and_rejects_overlong);
    RUN_TEST(test_histogram_has_tag_is_exact_without_truncation_collision);
    RUN_TEST(test_deserialize_terminates_an_unterminated_stored_tag);
    RUN_TEST(test_round_trip_covers_every_band);
    RUN_TEST(test_serialize_fails_loud_on_a_short_buffer);
    RUN_TEST(test_deserialize_rejects_bad_magic_version_and_length);
    return UNITY_END();
}
