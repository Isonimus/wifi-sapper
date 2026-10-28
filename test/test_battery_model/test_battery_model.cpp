/**
 * @file test_battery_model.cpp
 * @brief Native unit tests for the pure battery interpretation (ADR-0091, §4 #40).
 *
 * The seam forwards raw calibrated millivolts; everything that turns that into a charge bucket is here and
 * asserted on the host lane: the divider scaling, the coarse Li-ion buckets at their boundaries, the
 * hysteresis that stops a boundary-sitting voltage from flickering the icon, and the fail-safe that an
 * absent gauge or an uncalibrated divider reports Unknown rather than a guessed level.
 */
#include <unity.h>

#include "surface/battery_model.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

// --- divider scaling -------------------------------------------------------------------------------

void test_divider_scales_pin_to_cell_millivolts(void) {
    TEST_ASSERT_EQUAL_UINT32(4000, batteryCellMilliVolts(2000, 2000));  // 1:2 divider, x2.000.
    TEST_ASSERT_EQUAL_UINT32(3700, batteryCellMilliVolts(1850, 2000));
    TEST_ASSERT_EQUAL_UINT32(0, batteryCellMilliVolts(2000, 0));        // uncalibrated: no meaningful voltage.
}

void test_a_bad_divider_fails_loud_not_silently_truncated(void) {
    // A 10x divider typo (20000 for the correct 2000) must NOT wrap a uint16 to a plausible 464 mV and
    // then bucket to a believable Critical: the value stays an absurd 66000 mV (loud: the [BATT] line and
    // verify:battery's range check reject it), and the bucket is the obviously-wrong Full, not Critical.
    TEST_ASSERT_EQUAL_UINT32(66000, batteryCellMilliVolts(3300, 20000));
    TEST_ASSERT_EQUAL(BatteryLevel::Full, batteryLevelFromCell(66000, BatteryLevel::Unknown));
}

// --- plain buckets at their boundaries (previous == Unknown skips hysteresis) -----------------------

void test_buckets_at_their_boundaries(void) {
    TEST_ASSERT_EQUAL(BatteryLevel::Full, batteryLevelFromCell(4000, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::High, batteryLevelFromCell(3999, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::High, batteryLevelFromCell(3800, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Mid, batteryLevelFromCell(3799, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Mid, batteryLevelFromCell(3600, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Low, batteryLevelFromCell(3599, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Low, batteryLevelFromCell(3400, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Critical, batteryLevelFromCell(3399, BatteryLevel::Unknown));
    TEST_ASSERT_EQUAL(BatteryLevel::Critical, batteryLevelFromCell(3000, BatteryLevel::Unknown));
}

// --- hysteresis ------------------------------------------------------------------------------------

void test_hysteresis_holds_the_bucket_inside_the_band_when_rising(void) {
    // From Mid, the plain bucket at 3820 mV is High (>= 3800), but it is inside the +40 mV band above the
    // 3800 boundary, so the bucket must stay Mid until the reading clears 3840 mV.
    TEST_ASSERT_EQUAL(BatteryLevel::Mid, batteryLevelFromCell(3820, BatteryLevel::Mid));
    TEST_ASSERT_EQUAL(BatteryLevel::Mid, batteryLevelFromCell(3839, BatteryLevel::Mid));
    TEST_ASSERT_EQUAL(BatteryLevel::High, batteryLevelFromCell(3840, BatteryLevel::Mid));
}

void test_hysteresis_holds_the_bucket_inside_the_band_when_falling(void) {
    // From High, the plain bucket at 3780 mV is Mid (< 3800), but it is inside the -40 mV band below the
    // 3800 boundary, so the bucket must stay High until the reading drops below 3760 mV.
    TEST_ASSERT_EQUAL(BatteryLevel::High, batteryLevelFromCell(3780, BatteryLevel::High));
    TEST_ASSERT_EQUAL(BatteryLevel::High, batteryLevelFromCell(3760, BatteryLevel::High));
    TEST_ASSERT_EQUAL(BatteryLevel::Mid, batteryLevelFromCell(3759, BatteryLevel::High));
}

void test_a_large_jump_snaps_past_the_band(void) {
    // A cell swap (Critical -> a full pack) must not be stuck one step below by hysteresis: a reading well
    // clear of the crossed boundary snaps straight to the plain bucket.
    TEST_ASSERT_EQUAL(BatteryLevel::Full, batteryLevelFromCell(4100, BatteryLevel::Critical));
    TEST_ASSERT_EQUAL(BatteryLevel::Critical, batteryLevelFromCell(3100, BatteryLevel::Full));
}

// --- the full entry point (fail-safe cases) --------------------------------------------------------

void test_absent_gauge_is_unknown(void) {
    BatteryReading absent;  // present defaults to false.
    TEST_ASSERT_EQUAL(BatteryLevel::Unknown, batteryLevel(absent, 2000, BatteryLevel::High));
}

void test_uncalibrated_divider_is_unknown(void) {
    BatteryReading present;
    present.present = true;
    present.pinMilliVolts = 2000;  // would be 4000 mV cell at x2, but the divider is unknown.
    TEST_ASSERT_EQUAL(BatteryLevel::Unknown, batteryLevel(present, 0, BatteryLevel::High));
}

void test_present_calibrated_reading_buckets(void) {
    BatteryReading present;
    present.present = true;
    present.pinMilliVolts = 2000;  // x2.000 -> 4000 mV cell -> Full.
    TEST_ASSERT_EQUAL(BatteryLevel::Full, batteryLevel(present, 2000, BatteryLevel::Unknown));
    present.pinMilliVolts = 1750;  // -> 3500 mV -> Low.
    TEST_ASSERT_EQUAL(BatteryLevel::Low, batteryLevel(present, 2000, BatteryLevel::Unknown));
}

void test_level_names(void) {
    TEST_ASSERT_EQUAL_STRING("FULL", batteryLevelName(BatteryLevel::Full));
    TEST_ASSERT_EQUAL_STRING("CRITICAL", batteryLevelName(BatteryLevel::Critical));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", batteryLevelName(BatteryLevel::Unknown));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_divider_scales_pin_to_cell_millivolts);
    RUN_TEST(test_a_bad_divider_fails_loud_not_silently_truncated);
    RUN_TEST(test_buckets_at_their_boundaries);
    RUN_TEST(test_hysteresis_holds_the_bucket_inside_the_band_when_rising);
    RUN_TEST(test_hysteresis_holds_the_bucket_inside_the_band_when_falling);
    RUN_TEST(test_a_large_jump_snaps_past_the_band);
    RUN_TEST(test_absent_gauge_is_unknown);
    RUN_TEST(test_uncalibrated_divider_is_unknown);
    RUN_TEST(test_present_calibrated_reading_buckets);
    RUN_TEST(test_level_names);
    return UNITY_END();
}
