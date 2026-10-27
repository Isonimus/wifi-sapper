/**
 * @file test_button_hold.cpp
 * @brief Native unit tests for the pure button-hold entry gesture (ADR-0078, slice-0079).
 *
 * Proves slice-0079 Definition-of-Done scenario 3 (the pure gesture): `decideButtonHold` fires true
 * exactly once on the tick a continuous hold first reaches the threshold, never before it, never again
 * while still held, and resets on release so a later hold fires again; a pre-threshold bounce never
 * fires. This is a new pure unit (no prior logic to regress): the fail-before is the missing symbol
 * (the suite does not link without the ADR-0078 pure half), and it passes once it exists.
 */
#include <unity.h>

#include "config/button_hold.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

static constexpr uint32_t kHold = 2000;  // explicit threshold for readable arithmetic in the cases below.

/// The threshold tick fires exactly once; the rising edge and every still-held tick after do not.
void test_hold_fires_once_when_threshold_reached(void) {
    ButtonHoldState s;
    TEST_ASSERT_FALSE(decideButtonHold(true, 0, kHold, s));        // rising edge: start timer, no fire
    TEST_ASSERT_FALSE(decideButtonHold(true, 1999, kHold, s));     // one tick short of the threshold
    TEST_ASSERT_TRUE(decideButtonHold(true, 2000, kHold, s));      // threshold reached: fire once
    TEST_ASSERT_FALSE(decideButtonHold(true, 2001, kHold, s));     // still held: no re-fire
    TEST_ASSERT_FALSE(decideButtonHold(true, 9999, kHold, s));     // still held much later: still no re-fire
}

/// A press released before the threshold never fires, and neither does a bounce (a second short press).
void test_short_press_and_bounce_never_fire(void) {
    ButtonHoldState s;
    TEST_ASSERT_FALSE(decideButtonHold(true, 0, kHold, s));
    TEST_ASSERT_FALSE(decideButtonHold(true, 500, kHold, s));
    TEST_ASSERT_FALSE(decideButtonHold(false, 600, kHold, s));     // released early
    TEST_ASSERT_FALSE(decideButtonHold(true, 700, kHold, s));      // bounce: new rising edge
    TEST_ASSERT_FALSE(decideButtonHold(true, 1200, kHold, s));     // only 500ms into the bounce
    TEST_ASSERT_FALSE(decideButtonHold(false, 1300, kHold, s));
}

/// After firing and releasing, a fresh hold crossing the threshold fires again (state reset on release).
void test_release_resets_so_a_later_hold_fires_again(void) {
    ButtonHoldState s;
    TEST_ASSERT_FALSE(decideButtonHold(true, 0, kHold, s));
    TEST_ASSERT_TRUE(decideButtonHold(true, 2000, kHold, s));      // first hold fires
    TEST_ASSERT_FALSE(decideButtonHold(false, 2100, kHold, s));    // release resets
    TEST_ASSERT_FALSE(decideButtonHold(true, 2200, kHold, s));     // second hold: rising edge
    TEST_ASSERT_FALSE(decideButtonHold(true, 4199, kHold, s));     // one short of the second threshold
    TEST_ASSERT_TRUE(decideButtonHold(true, 4200, kHold, s));      // second hold fires
}

/// A fresh state polled with the button up never fires, and the rising edge starts the timer from the
/// current clock no matter how large it is (so a late first press still needs a full hold, not an
/// instant fire because `now >= holdMs`).
void test_fresh_and_late_first_press(void) {
    ButtonHoldState s;
    TEST_ASSERT_FALSE(decideButtonHold(false, 0, kHold, s));       // up: never fires
    TEST_ASSERT_FALSE(decideButtonHold(false, 123456, kHold, s));

    TEST_ASSERT_FALSE(decideButtonHold(true, 500000, kHold, s));   // late rising edge: timer starts at now
    TEST_ASSERT_FALSE(decideButtonHold(true, 501999, kHold, s));
    TEST_ASSERT_TRUE(decideButtonHold(true, 502000, kHold, s));    // fires a full hold after the edge
}

/// The elapsed computation is unsigned subtraction, so a hold that spans a millis() wrap still fires at
/// exactly `holdMs` of real elapsed time, not early or never.
void test_hold_spans_millis_wrap(void) {
    ButtonHoldState s;
    const uint32_t start = 0xFFFFFF00u;                            // 256 ticks before wrap
    TEST_ASSERT_FALSE(decideButtonHold(true, start, kHold, s));    // rising edge just before wrap
    TEST_ASSERT_FALSE(decideButtonHold(true, start + 1999u, kHold, s));  // wraps, elapsed 1999
    TEST_ASSERT_TRUE(decideButtonHold(true, start + 2000u, kHold, s));   // wraps, elapsed exactly 2000
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_hold_fires_once_when_threshold_reached);
    RUN_TEST(test_short_press_and_bounce_never_fire);
    RUN_TEST(test_release_resets_so_a_later_hold_fires_again);
    RUN_TEST(test_fresh_and_late_first_press);
    RUN_TEST(test_hold_spans_millis_wrap);
    return UNITY_END();
}
