/**
 * @file test_screen_sleep.cpp
 * @brief Native unit tests for the inactivity backlight dimmer (ADR-0093, §4).
 *
 * The whole policy is host-tested: the pure `inactivityStage` predicate at its boundaries (Active/Dimmed/
 * Off, disabled, dim-only, out-of-order, millis wrap) and the `ScreenSleepController` against a fake
 * IDisplay + fake clock (a capture/crack wakes, a routine sync does not, input wakes, setBrightness is
 * edge-only). The backlight-only invariant (never a CPU sleep) is structural: the controller touches only
 * IDisplay::setBrightness, which is all this test can drive.
 */
#include <unity.h>

#include <cstdio>

#include "net/cracked_result.h"
#include "net/cracked_sync.h"       // SyncOutcome
#include "support/fake_display.h"
#include "surface/screen_sleep.h"

using namespace sapper;
using sapper_test::FakeDisplay;

void setUp(void) {}
void tearDown(void) {}

static constexpr uint32_t kDim = 10000;  // dim after 10 s idle.
static constexpr uint32_t kOff = 30000;  // off after 30 s idle.

// --- pure predicate --------------------------------------------------------------------------------

void test_disabled_when_dim_after_is_zero(void) {
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, inactivityStage(1000000, 0, 0, kOff));
}

void test_stages_at_their_boundaries(void) {
    const uint32_t a = 1000;  // lastActivity
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, inactivityStage(a + 9999, a, kDim, kOff));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, inactivityStage(a + 10000, a, kDim, kOff));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, inactivityStage(a + 29999, a, kDim, kOff));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Off, inactivityStage(a + 30000, a, kDim, kOff));
}

void test_dim_only_when_off_after_is_zero(void) {
    // off=0 means never fully off: stays Dimmed however long idle.
    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, inactivityStage(1000000, 0, kDim, 0));
}

void test_off_wins_on_an_out_of_order_config(void) {
    // A hand-edited config with off < dim must not strand the screen on: Off wins past its threshold.
    TEST_ASSERT_EQUAL(ScreenSleepStage::Off, inactivityStage(15000, 1000, /*dim*/ 30000, /*off*/ 10000));
}

void test_elapsed_is_wrap_safe(void) {
    // lastActivity just below the u32 max, now just after the wrap: a small real elapsed, still Active.
    const uint32_t a = 0xFFFFFF00u;  // ~4.29e9
    const uint32_t now = 1000;       // wrapped past 0: elapsed = 1000 - a = 1256 ms
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, inactivityStage(now, a, kDim, kOff));
}

// --- controller ------------------------------------------------------------------------------------

static AppEvent crack(CrackedResult& r) {
    std::snprintf(r.essid, sizeof(r.essid), "Net");
    return AppEvent::newPassword(r);
}
static AppEvent capture(CaptureFact& f) {
    std::snprintf(f.ssid, sizeof(f.ssid), "Net");
    return AppEvent::handshakeCaptured(f);
}

void test_begin_lights_full(void) {
    FakeDisplay d;
    ScreenSleepController c(d, kDim, kOff);
    c.begin(0);
    TEST_ASSERT_EQUAL_UINT8(kScreenFullBrightness, d.last());
}

void test_idle_dims_then_turns_off_edge_only(void) {
    FakeDisplay d;
    ScreenSleepController c(d, kDim, kOff);
    c.begin(0);
    const size_t afterBegin = d.brightnessCalls.size();

    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, c.tick(5000));
    TEST_ASSERT_EQUAL_size_t(afterBegin, d.brightnessCalls.size());  // no change, no new setBrightness.

    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, c.tick(11000));
    TEST_ASSERT_EQUAL_UINT8(kScreenDimBrightness, d.last());
    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, c.tick(12000));   // still dimmed: no new call (edge-only).
    TEST_ASSERT_EQUAL_UINT8(kScreenDimBrightness, d.last());

    TEST_ASSERT_EQUAL(ScreenSleepStage::Off, c.tick(31000));
    TEST_ASSERT_EQUAL_UINT8(0, d.last());
}

void test_a_capture_or_crack_wakes_the_screen(void) {
    FakeDisplay d;
    ScreenSleepController c(d, kDim, kOff);
    c.begin(0);
    c.tick(31000);  // -> Off
    TEST_ASSERT_EQUAL_UINT8(0, d.last());

    CaptureFact f = {};
    c.onAppEvent(capture(f));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, c.tick(32000));  // activity folded in -> full.
    TEST_ASSERT_EQUAL_UINT8(kScreenFullBrightness, d.last());

    c.tick(43000);  // 11 s idle again -> dimmed
    TEST_ASSERT_EQUAL_UINT8(kScreenDimBrightness, d.last());
    CrackedResult r = {};
    c.onAppEvent(crack(r));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, c.tick(44000));
    TEST_ASSERT_EQUAL_UINT8(kScreenFullBrightness, d.last());
}

void test_a_routine_sync_does_not_wake_the_screen(void) {
    FakeDisplay d;
    ScreenSleepController c(d, kDim, kOff);
    c.begin(0);
    c.tick(11000);  // -> Dimmed
    TEST_ASSERT_EQUAL_UINT8(kScreenDimBrightness, d.last());

    SyncOutcome o = {};
    o.ok = true;
    c.onAppEvent(AppEvent::syncCompleted(o));
    TEST_ASSERT_EQUAL(ScreenSleepStage::Dimmed, c.tick(12000));  // sync is not activity: stays dimmed.
    TEST_ASSERT_EQUAL_UINT8(kScreenDimBrightness, d.last());
}

void test_note_input_wakes_the_screen(void) {
    FakeDisplay d;
    ScreenSleepController c(d, kDim, kOff);
    c.begin(0);
    c.tick(31000);  // -> Off
    TEST_ASSERT_EQUAL_UINT8(0, d.last());

    c.noteInput(31500);
    TEST_ASSERT_EQUAL(ScreenSleepStage::Active, c.tick(32000));  // idle timer reset by input -> full.
    TEST_ASSERT_EQUAL_UINT8(kScreenFullBrightness, d.last());
}

void test_stage_names(void) {
    TEST_ASSERT_EQUAL_STRING("active", screenSleepStageName(ScreenSleepStage::Active));
    TEST_ASSERT_EQUAL_STRING("dimmed", screenSleepStageName(ScreenSleepStage::Dimmed));
    TEST_ASSERT_EQUAL_STRING("off", screenSleepStageName(ScreenSleepStage::Off));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_disabled_when_dim_after_is_zero);
    RUN_TEST(test_stages_at_their_boundaries);
    RUN_TEST(test_dim_only_when_off_after_is_zero);
    RUN_TEST(test_off_wins_on_an_out_of_order_config);
    RUN_TEST(test_elapsed_is_wrap_safe);
    RUN_TEST(test_begin_lights_full);
    RUN_TEST(test_idle_dims_then_turns_off_edge_only);
    RUN_TEST(test_a_capture_or_crack_wakes_the_screen);
    RUN_TEST(test_a_routine_sync_does_not_wake_the_screen);
    RUN_TEST(test_note_input_wakes_the_screen);
    RUN_TEST(test_stage_names);
    return UNITY_END();
}
