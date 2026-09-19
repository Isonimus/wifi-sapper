/**
 * @file test_event_bus.cpp
 * @brief Native unit tests for the surface event bus (ADR-0021).
 *
 * Proves the routing contract surfaces rely on: every subscriber sees every published event, in
 * subscription order; the typed payloads route to the right field; a full or double subscribe fails
 * loud rather than silently dropping or double-delivering a surface; and an empty bus is a safe no-op.
 */
#include <unity.h>

#include <vector>

#include "core/event_bus.h"
#include "net/cracked_result.h"
#include "net/cracked_sync.h"
#include "net/upload_supervisor.h"
#include "support/recording_event_sink.h"

using namespace sapper;
using sapper_test::RecordingEventSink;

void setUp(void) {}
void tearDown(void) {}

// Records the order in which it was delivered to, relative to its peers, via a shared counter.
class OrderSink : public EventSink {
public:
    OrderSink(int id, std::vector<int>& log) : id_(id), log_(log) {}
    void onAppEvent(const AppEvent&) override { log_.push_back(id_); }

private:
    int id_;
    std::vector<int>& log_;
};

void test_publish_reaches_every_subscriber_in_subscription_order(void) {
    EventBus bus;
    std::vector<int> log;
    OrderSink a(1, log), b(2, log), c(3, log);
    TEST_ASSERT_TRUE(bus.subscribe(a));
    TEST_ASSERT_TRUE(bus.subscribe(b));
    TEST_ASSERT_TRUE(bus.subscribe(c));

    bus.publish(AppEvent::drainStarted());

    TEST_ASSERT_EQUAL_size_t(3, log.size());
    TEST_ASSERT_EQUAL_INT(1, log[0]);
    TEST_ASSERT_EQUAL_INT(2, log[1]);
    TEST_ASSERT_EQUAL_INT(3, log[2]);  // delivery follows subscription order.
}

void test_empty_bus_publish_is_a_safe_no_op(void) {
    EventBus bus;
    bus.publish(AppEvent::drainStarted());  // must not crash with no subscribers.
    TEST_ASSERT_EQUAL_size_t(0, bus.subscriberCount());
}

void test_typed_payloads_route_to_the_right_field(void) {
    EventBus bus;
    RecordingEventSink sink;
    bus.subscribe(sink);

    DrainOutcome drain;
    drain.accepted = 4;
    SyncOutcome sync;
    sync.downloaded = 7;
    CrackedResult pw;
    pw.essid[0] = 'X';

    bus.publish(AppEvent::drainStarted());
    bus.publish(AppEvent::drainCompleted(drain));
    bus.publish(AppEvent::syncCompleted(sync));
    bus.publish(AppEvent::newPassword(pw));
    bus.publish(AppEvent::firstSyncSummary(9));

    TEST_ASSERT_EQUAL_INT(1, sink.drainStarted);
    TEST_ASSERT_EQUAL_size_t(1, sink.drainsCompleted.size());
    TEST_ASSERT_EQUAL_size_t(4, sink.drainsCompleted[0].accepted);
    TEST_ASSERT_EQUAL_size_t(1, sink.outcomes.size());
    TEST_ASSERT_EQUAL_size_t(7, sink.outcomes[0].downloaded);
    TEST_ASSERT_EQUAL_size_t(1, sink.newPasswords.size());
    TEST_ASSERT_EQUAL_INT('X', sink.newPasswords[0].essid[0]);
    TEST_ASSERT_EQUAL_INT(1, sink.summaries);
    TEST_ASSERT_EQUAL_size_t(9, sink.lastSummaryCount);
}

void test_double_subscribe_is_rejected_so_events_are_not_doubled(void) {
    EventBus bus;
    RecordingEventSink sink;
    TEST_ASSERT_TRUE(bus.subscribe(sink));
    TEST_ASSERT_FALSE(bus.subscribe(sink));  // already subscribed — refused, loud.

    bus.publish(AppEvent::firstSyncSummary(1));

    TEST_ASSERT_EQUAL_size_t(1, bus.subscriberCount());
    TEST_ASSERT_EQUAL_INT(1, sink.summaries);  // delivered once, not twice.
}

void test_subscribe_past_capacity_fails_loud(void) {
    EventBus bus;
    // The bus reserves a fixed number of slots; fill them, then prove the overflow is refused rather
    // than silently dropped. Six sinks fills the current capacity.
    std::vector<int> log;
    OrderSink s0(0, log), s1(1, log), s2(2, log), s3(3, log), s4(4, log), s5(5, log), s6(6, log);
    TEST_ASSERT_TRUE(bus.subscribe(s0));
    TEST_ASSERT_TRUE(bus.subscribe(s1));
    TEST_ASSERT_TRUE(bus.subscribe(s2));
    TEST_ASSERT_TRUE(bus.subscribe(s3));
    TEST_ASSERT_TRUE(bus.subscribe(s4));
    TEST_ASSERT_TRUE(bus.subscribe(s5));
    TEST_ASSERT_FALSE(bus.subscribe(s6));  // capacity exceeded — refused so the caller can fail boot.
    TEST_ASSERT_EQUAL_size_t(6, bus.subscriberCount());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_publish_reaches_every_subscriber_in_subscription_order);
    RUN_TEST(test_empty_bus_publish_is_a_safe_no_op);
    RUN_TEST(test_typed_payloads_route_to_the_right_field);
    RUN_TEST(test_double_subscribe_is_rejected_so_events_are_not_doubled);
    RUN_TEST(test_subscribe_past_capacity_fails_loud);
    return UNITY_END();
}
