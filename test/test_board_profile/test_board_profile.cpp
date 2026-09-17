/**
 * @file test_board_profile.cpp
 * @brief Native unit tests for the Board Profile and display-HAL resolution (ADR-0002).
 *
 * Proves slice-0005 Definition-of-Done Scenario A (capability half): the Cardputer ADV
 * profile reports the expected capabilities, the display HAL resolves to the real impl when
 * a board has a panel and to the null impl when it does not, and the null display behaves
 * inertly. The real LGFX impl is device-only and proven on hardware (Scenario C), so it is
 * not linked here.
 */
#include <unity.h>

#include "config/boards/cardputer.h"
#include "hal/display/display_hal.h"
#include "hal/display/null_display.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

void test_cardputer_capabilities(void) {
    TEST_ASSERT_TRUE(kCardputerProfile.hasDisplay);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputKind::Keyboard),
                          static_cast<int>(kCardputerProfile.input));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(StorageKind::Sd),
                          static_cast<int>(kCardputerProfile.storage));
    TEST_ASSERT_FALSE(kCardputerProfile.hasPsram);
    TEST_ASSERT_EQUAL_UINT16(240, kCardputerProfile.display.width);
    TEST_ASSERT_EQUAL_UINT16(135, kCardputerProfile.display.height);
    TEST_ASSERT_EQUAL_UINT8(16, kCardputerProfile.display.colorBits);
}

void test_resolve_real_when_board_has_display(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DisplayResolution::Real),
                          static_cast<int>(resolveDisplay(kCardputerProfile)));
}

void test_resolve_null_when_board_screenless(void) {
    BoardProfile screenless = kCardputerProfile;
    screenless.hasDisplay = false;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DisplayResolution::Null),
                          static_cast<int>(resolveDisplay(screenless)));
}

void test_null_display_is_inert(void) {
    NullDisplay display;
    TEST_ASSERT_TRUE(display.begin());  // absence of a panel is not a failure
    TEST_ASSERT_EQUAL_UINT16(0, display.width());
    TEST_ASSERT_EQUAL_UINT16(0, display.height());
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(display.canvasByteLength()));
    uint8_t buffer[8];
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(display.readCanvas(0, buffer, sizeof(buffer))));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cardputer_capabilities);
    RUN_TEST(test_resolve_real_when_board_has_display);
    RUN_TEST(test_resolve_null_when_board_screenless);
    RUN_TEST(test_null_display_is_inert);
    return UNITY_END();
}
