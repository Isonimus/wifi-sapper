/**
 * @file test_entry_gesture.cpp
 * @brief Native unit tests for the Maintenance-entry debounce (ADR-0053, slice-0054).
 *
 * Proves slice-0054 Definition-of-Done Scenario A: an isolated glitch on the entry (strapping) pin
 * never confirms a press, but a real press (kEntryDebounceSamples consecutive LOWs) does. These fail
 * against the superseded single-sample gesture (ADR-0039 decision 2), which had no debounce at all —
 * the first LOW, glitch or not, entered Maintenance.
 */
#include <unity.h>

#include "config/entry_gesture.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

/// A lone LOW between HIGHs is a glitch, not a press: the run resets on each HIGH, so with the default
/// threshold no isolated sample ever confirms. This is exactly what the old single-sample seam got
/// wrong — it would have entered Maintenance on that first LOW.
void test_isolated_glitch_never_confirms(void) {
    PressDebouncer debounce(kEntryDebounceSamples);
    TEST_ASSERT_FALSE(debounce.feed(false));  // released
    TEST_ASSERT_FALSE(debounce.feed(true));   // a single glitch LOW — old code would have fired here
    TEST_ASSERT_FALSE(debounce.feed(false));  // released again: run resets
    TEST_ASSERT_FALSE(debounce.feed(true));   // another lone LOW
    TEST_ASSERT_FALSE(debounce.feed(false));
}

/// kEntryDebounceSamples consecutive LOWs confirm a real press — and not one sample sooner.
void test_consecutive_run_confirms_at_threshold(void) {
    PressDebouncer debounce(kEntryDebounceSamples);
    for (uint8_t i = 1; i < kEntryDebounceSamples; ++i) {
        TEST_ASSERT_FALSE(debounce.feed(true));  // below threshold: not yet confirmed
    }
    TEST_ASSERT_TRUE(debounce.feed(true));  // the kEntryDebounceSamples-th consecutive LOW confirms
}

/// A release partway through a run resets it: LOW,LOW,HIGH must not carry credit into the next LOW.
void test_release_resets_the_run(void) {
    PressDebouncer debounce(kEntryDebounceSamples);  // threshold 3
    TEST_ASSERT_FALSE(debounce.feed(true));
    TEST_ASSERT_FALSE(debounce.feed(true));   // two in a row, one short
    TEST_ASSERT_FALSE(debounce.feed(false));  // release: run back to zero
    TEST_ASSERT_FALSE(debounce.feed(true));   // this is the 1st of a new run, not the 3rd
    TEST_ASSERT_FALSE(debounce.feed(true));   // 2nd
    TEST_ASSERT_TRUE(debounce.feed(true));    // 3rd — now confirmed
}

/// A normal boot (nobody pressing) feeds only HIGH for the whole window and never confirms — the poll
/// loop would fall through to the timeout and boot to Station.
void test_all_released_never_confirms(void) {
    PressDebouncer debounce(kEntryDebounceSamples);
    const uint32_t samples = kMaintenanceEntryWindowMs / kEntryPollIntervalMs;
    for (uint32_t i = 0; i < samples; ++i) {
        TEST_ASSERT_FALSE(debounce.feed(false));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_isolated_glitch_never_confirms);
    RUN_TEST(test_consecutive_run_confirms_at_threshold);
    RUN_TEST(test_release_resets_the_run);
    RUN_TEST(test_all_released_never_confirms);
    return UNITY_END();
}
