/**
 * @file test_wordmark.cpp
 * @brief Native unit tests for the product wordmark with a magenta drop shadow (slice-0097).
 *
 * drawWordmark is pure over the IDisplay seam, so its colour/offset/layer-order logic is host-tested against
 * a recording FakeDisplay (no LovyanGFX). The on-panel layout stays photographic like the renderers.
 */
#include <unity.h>

#include "support/fake_display.h"
#include "surface/wordmark.h"

using sapper::drawWordmark;
using sapper_test::FakeDisplay;

namespace {
constexpr uint16_t kMagenta = 0xF81F;
constexpr uint16_t kWhite = 0xFFFF;
}  // namespace

void setUp(void) {}
void tearDown(void) {}

// A size-4 title (the direct-panel tier) drops its shadow by 4/2 = 2 px down-right in magenta, then draws the
// white glyphs last so they sit on top of the shadow. No cyan channel.
void test_shadow_layers_and_order(void) {
    FakeDisplay d;
    drawWordmark(d, 100, 50, "WiFi Sapper", 4);

    TEST_ASSERT_EQUAL_UINT(2, d.drawTextCalls.size());

    // magenta drop shadow, offset down-right
    TEST_ASSERT_EQUAL_INT32(102, d.drawTextCalls[0].x);
    TEST_ASSERT_EQUAL_INT32(52, d.drawTextCalls[0].y);
    TEST_ASSERT_EQUAL_HEX16(kMagenta, d.drawTextCalls[0].color);

    // white glyphs, centred, drawn LAST (on top of the shadow)
    TEST_ASSERT_EQUAL_INT32(100, d.drawTextCalls[1].x);
    TEST_ASSERT_EQUAL_INT32(50, d.drawTextCalls[1].y);
    TEST_ASSERT_EQUAL_HEX16(kWhite, d.drawTextCalls[1].color);

    // the same text and size across both layers (a shadow, not two different draws)
    for (const auto& call : d.drawTextCalls) {
        TEST_ASSERT_EQUAL_STRING("WiFi Sapper", call.text.c_str());
        TEST_ASSERT_EQUAL_UINT8(4, call.size);
    }
}

// The offset scales with glyph size (size-3 -> 1 px) and is floored at 1 so the shadow never collapses onto
// the glyphs at size 1.
void test_offset_scales_and_never_zero(void) {
    FakeDisplay d3;
    drawWordmark(d3, 10, 20, "WiFi Sapper", 3);
    TEST_ASSERT_EQUAL_INT32(11, d3.drawTextCalls[0].x);  // 10 + 1 (3/2 = 1)
    TEST_ASSERT_EQUAL_INT32(21, d3.drawTextCalls[0].y);  // 20 + 1

    FakeDisplay d1;
    drawWordmark(d1, 10, 20, "x", 1);
    TEST_ASSERT_EQUAL_INT32(11, d1.drawTextCalls[0].x);  // 10 + 1 (floored at 1, not 0)
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_shadow_layers_and_order);
    RUN_TEST(test_offset_scales_and_never_zero);
    return UNITY_END();
}
