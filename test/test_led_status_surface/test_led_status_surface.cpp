/**
 * @file test_led_status_surface.cpp
 * @brief Native unit tests for the status-LED surface policy (ADR-0021).
 *
 * The LED policy is pure and clock-driven, so all of it is asserted here against a fake driver and a
 * fake clock: the alive heartbeat blinks the base colour; drain facts map to hunting/working/degraded/
 * fault; a hard fault sits solid (an alarm, not a heartbeat); and a new password latches a solid flash
 * that overrides the heartbeat, then releases back to it. The physical WS2812 colour is the on-air
 * verify's job (Scenario, lane 3); the decision of *what* to show is proven here.
 */
#include <unity.h>

#include <algorithm>

#include "net/cracked_result.h"     // CrackedResult (the NewPassword payload constructed below)
#include "net/upload_supervisor.h"  // DrainOutcome
#include "support/fake_led_driver.h"
#include "surface/led_status_surface.h"

using namespace sapper;
using sapper_test::FakeLedDriver;

void setUp(void) {}
void tearDown(void) {}

static DrainOutcome cleanDrain() {
    DrainOutcome o;
    o.ran = true;
    o.associated = true;  // all counts zero: a clean cycle (also the shape of a healthy sync-only window).
    return o;
}

static bool contains(const std::vector<LedStatus>& v, LedStatus s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

void test_begin_lights_the_hunting_heartbeat(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);

    surface.begin(0);

    TEST_ASSERT_EQUAL_size_t(1, led.shown.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));
}

void test_base_status_blinks_as_a_heartbeat(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);  // -> Hunting (lit half).

    surface.tick(500);   // still lit half — no transition.
    surface.tick(1000);  // dark half -> Off.
    surface.tick(1500);  // still dark — no transition.
    surface.tick(2000);  // lit half again -> Hunting.

    // Exactly three transitions: on, off, on — proof the firmware is looping, not hung solid.
    TEST_ASSERT_EQUAL_size_t(3, led.shown.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.shown[0]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Off), static_cast<int>(led.shown[1]));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.shown[2]));
}

void test_drain_started_shows_working(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);

    surface.onAppEvent(AppEvent::drainStarted());
    surface.tick(0);  // lit half -> Working.

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Working), static_cast<int>(led.last()));
}

void test_clean_drain_returns_to_hunting(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);
    surface.onAppEvent(AppEvent::drainStarted());
    surface.tick(0);  // Working.

    const DrainOutcome drain = cleanDrain();
    surface.onAppEvent(AppEvent::drainCompleted(drain));
    surface.tick(10);  // lit half -> Hunting.

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));
}

void test_sync_only_clean_window_is_not_degraded(void) {
    // A healthy hourly sync opens an STA window with no uploads: associated, every upload count zero.
    // That must read as clean (Hunting), not as a warning — guards statusFromDrain's clean definition.
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);

    surface.onAppEvent(AppEvent::drainCompleted(cleanDrain()));
    surface.tick(10);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));
}

void test_offline_cycle_shows_degraded(void) {
    // Degraded is the connectivity signal: the cycle could not associate, so uploads and the hourly sync
    // are stalled until the network is reachable — the one state an operator must act on.
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);

    DrainOutcome offline;  // never associated: an offline/out-of-range cycle.
    offline.ran = true;
    surface.onAppEvent(AppEvent::drainCompleted(offline));
    surface.tick(10);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Degraded), static_cast<int>(led.last()));
}

void test_online_cycle_with_data_issues_still_shows_hunting(void) {
    // A rejected handshake, a purged corrupt capture, or a store error is a data-level outcome, not a
    // connectivity failure. As long as the appliance associated, the coarse status stays Hunting — else a
    // routine rejection (partial captures, wpa-sec dedup) would make a healthy, online board read amber
    // almost always (slice-0022 Scenario J on-air finding). Those details live in the serial log and the
    // later web dashboard, not on this one-colour indicator.
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);

    DrainOutcome online = cleanDrain();  // associated, but with data-level problems left over.
    online.rejected = 2;
    online.purged = 1;
    online.storeErrors = 1;
    surface.onAppEvent(AppEvent::drainCompleted(online));
    surface.tick(10);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));
}

void test_resume_failure_sits_solid_as_a_fault(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);  // Hunting.

    DrainOutcome fault = cleanDrain();
    fault.resumeFailed = true;
    surface.onAppEvent(AppEvent::drainCompleted(fault));
    surface.tick(0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Fault), static_cast<int>(led.last()));

    surface.tick(1000);  // the dark half of a heartbeat — a Fault must NOT blink to Off.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Fault), static_cast<int>(led.last()));
    // No Off transition was ever emitted while in Fault.
    led.shown.erase(led.shown.begin());  // drop the initial Hunting from begin().
    TEST_ASSERT_FALSE(contains(led.shown, LedStatus::Off));
}

void test_new_password_latches_a_solid_flash_then_releases(void) {
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);  // Hunting.

    surface.onAppEvent(AppEvent::newPassword(CrackedResult{}));
    surface.tick(100);  // the flash arms and shows solid Recovered.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Recovered), static_cast<int>(led.last()));

    surface.tick(1000);  // a heartbeat dark half — the flash overrides it, staying solid.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Recovered), static_cast<int>(led.last()));
    TEST_ASSERT_FALSE(contains(led.shown, LedStatus::Off));  // never blinked during the hold.

    surface.tick(6000);  // past the 5 s hold and into a lit half -> heartbeat resumes.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));
}

void test_capture_gives_a_brief_flash_then_releases(void) {
    // A captured handshake flashes the LED briefly (ADR-0031 #4) — much shorter than the 5 s Recovered
    // latch, so continuous hunting does not strobe. Fail-before: without the HandshakeCaptured case the
    // LED never leaves the heartbeat. Armed at t=100, kCapturedHoldMs=800 -> until 900.
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);  // Hunting.

    surface.onAppEvent(AppEvent::handshakeCaptured(CaptureFact{}));
    surface.tick(100);  // the capture flash arms.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Captured), static_cast<int>(led.last()));

    surface.tick(500);  // still within the brief hold, overriding the heartbeat.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Captured), static_cast<int>(led.last()));

    surface.tick(2000);  // well past the 800 ms hold (a Recovered latch would still be solid here) ->
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Hunting), static_cast<int>(led.last()));  // heartbeat back.
}

void test_recovered_outranks_a_concurrent_capture_flash(void) {
    // When a capture and a crack are live at once, the rarer, bigger news wins the LED: Recovered
    // outranks Captured (ADR-0031 #4 render precedence).
    FakeLedDriver led;
    LedStatusSurface surface(led);
    surface.begin(0);  // Hunting.

    surface.onAppEvent(AppEvent::handshakeCaptured(CaptureFact{}));
    surface.onAppEvent(AppEvent::newPassword(CrackedResult{}));  // both latches arm this step.
    surface.tick(100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Recovered), static_cast<int>(led.last()));

    surface.tick(1000);  // past the capture hold but inside the 5 s Recovered latch — still Recovered.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LedStatus::Recovered), static_cast<int>(led.last()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_lights_the_hunting_heartbeat);
    RUN_TEST(test_base_status_blinks_as_a_heartbeat);
    RUN_TEST(test_drain_started_shows_working);
    RUN_TEST(test_clean_drain_returns_to_hunting);
    RUN_TEST(test_sync_only_clean_window_is_not_degraded);
    RUN_TEST(test_offline_cycle_shows_degraded);
    RUN_TEST(test_online_cycle_with_data_issues_still_shows_hunting);
    RUN_TEST(test_resume_failure_sits_solid_as_a_fault);
    RUN_TEST(test_new_password_latches_a_solid_flash_then_releases);
    RUN_TEST(test_capture_gives_a_brief_flash_then_releases);
    RUN_TEST(test_recovered_outranks_a_concurrent_capture_flash);
    return UNITY_END();
}
