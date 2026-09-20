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

/// A spy IDisplay that counts the draws routed to it, so a test can prove where panelAfterInit sent
/// them. Everything but the counters is inert — this stands in for the real panel without LovyanGFX.
class CountingDisplay final : public IDisplay {
public:
    int draws = 0;
    bool begin() override { return true; }
    uint16_t width() const override { return 240; }
    uint16_t height() const override { return 135; }
    void fillScreen(uint16_t) override { ++draws; }
    void drawPixel(int32_t, int32_t, uint16_t) override { ++draws; }
    void drawText(int32_t, int32_t, const char*, uint16_t, uint8_t) override { ++draws; }
    void present() override {}
    size_t canvasByteLength() const override { return 0; }
    size_t readCanvas(size_t, uint8_t*, size_t) const override { return 0; }
};

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

// ADR-0045 Scenario A: on a real panel init failure, a surface draw must land on the null fallback,
// never on the real (dead) panel. With the spy as `real` and a NullDisplay as fallback, a draw through
// the selection increments the spy only when init succeeded — a regression to returning `real`
// unconditionally would increment the spy in the failure case and fail this test.
void test_panel_after_init_routes_to_fallback_on_failure(void) {
    CountingDisplay real;
    NullDisplay fallback;

    IDisplay& onFailure = panelAfterInit(real, /*initOk=*/false, fallback);
    onFailure.fillScreen(0);
    onFailure.drawText(0, 0, "x", 0, 1);
    TEST_ASSERT_EQUAL_INT(0, real.draws);  // the draws went to the null fallback, not the dead panel

    IDisplay& onSuccess = panelAfterInit(real, /*initOk=*/true, fallback);
    onSuccess.fillScreen(0);
    onSuccess.drawText(0, 0, "x", 0, 1);
    TEST_ASSERT_EQUAL_INT(2, real.draws);  // a working panel still receives every draw
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_cardputer_capabilities);
    RUN_TEST(test_resolve_real_when_board_has_display);
    RUN_TEST(test_resolve_null_when_board_screenless);
    RUN_TEST(test_null_display_is_inert);
    RUN_TEST(test_panel_after_init_routes_to_fallback_on_failure);
    return UNITY_END();
}
