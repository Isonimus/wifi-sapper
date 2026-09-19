/**
 * @file test_sync_scheduler.cpp
 * @brief Native unit tests for the pure hourly sync cadence (ADR-0019 decision #6; slice-0020
 *        Scenario H, timing half).
 *
 * The window-sharing/forcing behaviour Scenario H also names is supervisor logic and is proved in
 * test_upload_supervisor; here we prove the timing primitive it rests on: due at boot, not due before
 * the interval, due after, reset on a successful sync, and wrap-safe across a millis() rollover.
 */
#include <unity.h>

#include <cstdint>

#include "net/sync_scheduler.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

void test_first_sync_is_due_at_boot(void) {
    SyncScheduler scheduler(1000);
    scheduler.begin(5000);
    TEST_ASSERT_TRUE(scheduler.isDue(5000));  // catch up on the first STA window, not an hour later.
}

void test_not_due_before_interval_after_a_sync(void) {
    SyncScheduler scheduler(1000);
    scheduler.begin(5000);
    scheduler.noteSynced(5000);
    TEST_ASSERT_FALSE(scheduler.isDue(5999));  // 1ms short of the interval.
}

void test_due_after_interval_after_a_sync(void) {
    SyncScheduler scheduler(1000);
    scheduler.begin(5000);
    scheduler.noteSynced(5000);
    TEST_ASSERT_TRUE(scheduler.isDue(6000));   // exactly the interval.
    TEST_ASSERT_TRUE(scheduler.isDue(600000)); // and well past it.
}

void test_note_synced_pushes_the_deadline_out(void) {
    SyncScheduler scheduler(1000);
    scheduler.begin(0);
    TEST_ASSERT_TRUE(scheduler.isDue(2000));  // due (past boot).
    scheduler.noteSynced(2000);
    TEST_ASSERT_FALSE(scheduler.isDue(2500)); // now not due until 3000.
    TEST_ASSERT_TRUE(scheduler.isDue(3000));
}

void test_wrap_safe_across_a_millis_rollover(void) {
    SyncScheduler scheduler(1000);
    // Sync just before the 32-bit rollover; the next deadline wraps past zero.
    const uint32_t nearMax = 0xFFFFFF00u;  // 4294967040
    scheduler.noteSynced(nearMax);         // dueAt = nearMax + 1000 wraps to 744.
    TEST_ASSERT_FALSE(scheduler.isDue(0xFFFFFFF0u));  // still before the wrapped deadline.
    TEST_ASSERT_TRUE(scheduler.isDue(744));           // at the wrapped deadline, after rollover.
    TEST_ASSERT_TRUE(scheduler.isDue(800));           // and just past it.
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_sync_is_due_at_boot);
    RUN_TEST(test_not_due_before_interval_after_a_sync);
    RUN_TEST(test_due_after_interval_after_a_sync);
    RUN_TEST(test_note_synced_pushes_the_deadline_out);
    RUN_TEST(test_wrap_safe_across_a_millis_rollover);
    return UNITY_END();
}
