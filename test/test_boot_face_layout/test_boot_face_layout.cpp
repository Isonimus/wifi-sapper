/**
 * @file test_boot_face_layout.cpp
 * @brief Native unit tests for the resolution-aware boot-face tier selector (slice-0104).
 *
 * Proves slice-0104 Definition-of-Done scenario 4: bootFaceLayoutFor returns compact for h < 160, roomy
 * for 160 <= h < 280, and large for h >= 280, at and across both boundaries. The selector lives in a pure
 * header because the boot faces it serves are in main.cpp, which the native lane cannot compile, so an
 * off-by-one in the threshold (which would silently drop the CYD's 480x320 into the roomy tier and
 * under-fill it, the exact bug this slice fixes) would otherwise be invisible to the test suite. This is a
 * new pure unit: the fail-before is the missing symbol (the suite does not link without the header), and
 * it passes once the selector exists. The per-tier coordinates are photo-tuned and verified on glass, not
 * here; the static_asserts in the header prove each tier's tallest face stays on its design panel.
 */
#include <unity.h>

#include "surface/boot_face_layout.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

/// The compact tier serves every panel below the roomy threshold, including the Cardputer/Stick 135 px.
void test_compact_below_roomy_threshold(void) {
    TEST_ASSERT_EQUAL_INT(kCompact.titleSize, bootFaceLayoutFor(135).titleSize);  // Cardputer/Stick
    TEST_ASSERT_EQUAL_INT(kCompact.titleSize, bootFaceLayoutFor(134).titleSize);
    TEST_ASSERT_EQUAL_INT(kCompact.titleSize, bootFaceLayoutFor(159).titleSize);  // one below the knee
    TEST_ASSERT_EQUAL_INT(kCompact.titleSize, bootFaceLayoutFor(0).titleSize);    // degenerate: never crashes
}

/// The roomy tier begins exactly at kRoomyMinHeight and serves up to one below the large threshold.
void test_roomy_between_thresholds(void) {
    TEST_ASSERT_EQUAL_INT(kRoomy.titleSize, bootFaceLayoutFor(160).titleSize);  // the knee itself is roomy
    TEST_ASSERT_EQUAL_INT(kRoomy.titleSize, bootFaceLayoutFor(170).titleSize);  // T-embed
    TEST_ASSERT_EQUAL_INT(kRoomy.titleSize, bootFaceLayoutFor(279).titleSize);  // one below the large knee
}

/// The large tier begins exactly at kLargeMinHeight and serves the CYD's 480x320.
void test_large_at_and_above_threshold(void) {
    TEST_ASSERT_EQUAL_INT(kLarge.titleSize, bootFaceLayoutFor(280).titleSize);  // the knee itself is large
    TEST_ASSERT_EQUAL_INT(kLarge.titleSize, bootFaceLayoutFor(320).titleSize);  // CYD
    TEST_ASSERT_EQUAL_INT(kLarge.titleSize, bootFaceLayoutFor(1024).titleSize);
}

/// The three tiers are genuinely distinct (not the same aggregate under different names), so a selection
/// mistake between any two is observable: each has a strictly larger title than the one below it.
void test_tiers_are_distinct_and_ordered(void) {
    TEST_ASSERT_TRUE(kCompact.titleSize < kRoomy.titleSize);
    TEST_ASSERT_TRUE(kRoomy.titleSize < kLarge.titleSize);
    TEST_ASSERT_TRUE(kLargeMinHeight > kRoomyMinHeight);
}

/// The tallest face (provisioning, slot 5) stays on-panel at every tier. This mirrors the header's
/// static_asserts at runtime so the proof is also visible in the test report, and guards the design
/// heights a future coordinate nudge must respect.
void test_provisioning_last_line_fits_each_tier(void) {
    TEST_ASSERT_TRUE(kCompact.slotBottom(kProvisioningLastSlot) <= 135);
    TEST_ASSERT_TRUE(kRoomy.slotBottom(kProvisioningLastSlot) <= 170);
    TEST_ASSERT_TRUE(kLarge.slotBottom(kProvisioningLastSlot) <= 320);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_compact_below_roomy_threshold);
    RUN_TEST(test_roomy_between_thresholds);
    RUN_TEST(test_large_at_and_above_threshold);
    RUN_TEST(test_tiers_are_distinct_and_ordered);
    RUN_TEST(test_provisioning_last_line_fits_each_tier);
    return UNITY_END();
}
