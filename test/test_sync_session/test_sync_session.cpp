/**
 * @file test_sync_session.cpp
 * @brief Native unit tests for the ScheduledSyncSession binding (ADR-0019 decision #7; slice-0020
 *        Scenario H/I glue).
 *
 * ScheduledSyncSession is the thin seam between the pure SyncScheduler and the pure CrackedSync that the
 * UploadSupervisor drives. Its one piece of logic beyond delegation is the cadence rule the supervisor
 * must not have to know: a *successful* sync advances the hourly deadline, a *failed* one leaves it due
 * so the next STA window retries rather than waiting a full hour. These prove exactly that, over the real
 * scheduler and sync with fake network/storage.
 */
#include <unity.h>

#include "core/event_bus.h"
#include "net/sync_session.h"
#include "support/fake_cracked_fetcher.h"
#include "support/fake_cracked_store.h"

using namespace sapper;
using sapper_test::FakeCrackedFetcher;
using sapper_test::FakeCrackedStore;

static const uint32_t kTestIntervalMs = 1000;

void setUp(void) {}
void tearDown(void) {}

void test_a_successful_sync_advances_the_hourly_cadence(void) {
    FakeCrackedStore store;
    FakeCrackedFetcher fetcher;
    fetcher.lines = {"aabbccddee01:001122334455:Net:pw1"};  // one result -> a successful sync.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;  // an empty bus: these tests assert cadence, not events.
    CrackedSync sync(fetcher, manifest, "K", bus);
    SyncScheduler scheduler(kTestIntervalMs);
    ScheduledSyncSession session(scheduler, sync);

    session.begin(0);
    TEST_ASSERT_TRUE(session.due(0));   // due at boot so the first window catches up.

    session.runInWindow(0);             // success -> the deadline is pushed out one interval.

    TEST_ASSERT_FALSE(session.due(kTestIntervalMs - 1));  // not due before the interval elapses,
    TEST_ASSERT_TRUE(session.due(kTestIntervalMs));       // due again exactly one interval later.
}

void test_a_failed_sync_leaves_the_cadence_due_for_the_next_window(void) {
    FakeCrackedStore store;
    FakeCrackedFetcher fetcher;
    fetcher.result = FetchResult::Transport;  // a transport failure: runSync returns ok=false.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;  // an empty bus: these tests assert cadence, not events.
    CrackedSync sync(fetcher, manifest, "K", bus);
    SyncScheduler scheduler(kTestIntervalMs);
    ScheduledSyncSession session(scheduler, sync);

    session.begin(0);
    TEST_ASSERT_TRUE(session.due(0));

    session.runInWindow(0);  // fails -> the cadence must NOT advance (ADR-0019 decision #7).

    TEST_ASSERT_TRUE(session.due(0));                    // still due immediately after,
    TEST_ASSERT_TRUE(session.due(kTestIntervalMs - 1));  // and still due well before an interval would pass
                                                         // (which is what proves noteSynced was NOT called).
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_successful_sync_advances_the_hourly_cadence);
    RUN_TEST(test_a_failed_sync_leaves_the_cadence_due_for_the_next_window);
    return UNITY_END();
}
