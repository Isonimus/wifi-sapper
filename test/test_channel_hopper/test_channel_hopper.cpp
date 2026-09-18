/**
 * @file test_channel_hopper.cpp
 * @brief Native unit tests for the pure channel-hop schedule (slice-0014 Scenario A, ADR-0013).
 *
 * The hopper has no radio and no clock of its own: the test supplies the time, so the whole sweep is
 * deterministic. It proves the schedule dwells for the configured interval, advances on the boundary
 * (not before), and wraps at the end.
 */
#include <unity.h>

#include <cstdint>

#include "net/channel_hopper.h"

using namespace sapper;

static const uint8_t kChannels[3] = {1, 6, 11};
static constexpr uint32_t kDwellMs = 300;

void setUp(void) {}
void tearDown(void) {}

void test_starts_on_first_channel(void) {
    ChannelHopper hopper(kChannels, 3, kDwellMs);
    TEST_ASSERT_EQUAL_UINT8(1, hopper.currentChannel());
    TEST_ASSERT_EQUAL_UINT32(3, hopper.channelCount());
}

void test_holds_until_dwell_elapses_then_advances(void) {
    ChannelHopper hopper(kChannels, 3, kDwellMs);
    hopper.reset(0);

    // Before the dwell boundary: no hop, still on channel 1.
    TEST_ASSERT_FALSE(hopper.tick(1));
    TEST_ASSERT_FALSE(hopper.tick(299));
    TEST_ASSERT_EQUAL_UINT8(1, hopper.currentChannel());

    // At the boundary: hop to channel 6.
    TEST_ASSERT_TRUE(hopper.tick(300));
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
}

void test_sweeps_and_wraps(void) {
    ChannelHopper hopper(kChannels, 3, kDwellMs);
    hopper.reset(0);

    TEST_ASSERT_TRUE(hopper.tick(300));   // 1 → 6
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
    TEST_ASSERT_TRUE(hopper.tick(600));   // 6 → 11
    TEST_ASSERT_EQUAL_UINT8(11, hopper.currentChannel());
    TEST_ASSERT_TRUE(hopper.tick(900));   // 11 → 1 (wrap)
    TEST_ASSERT_EQUAL_UINT8(1, hopper.currentChannel());
}

void test_dwell_measured_from_last_hop_not_start(void) {
    // Each dwell is measured from the previous hop, so an over-long gap still advances exactly one
    // channel — the schedule never "catches up" by skipping channels.
    ChannelHopper hopper(kChannels, 3, kDwellMs);
    hopper.reset(0);
    TEST_ASSERT_TRUE(hopper.tick(1000));  // long past one dwell: hop once, to channel 6.
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
    TEST_ASSERT_FALSE(hopper.tick(1001));  // only 1ms since that hop: no further hop.
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
}

void test_single_channel_sweep_hops_onto_itself(void) {
    // A one-channel sweep still reports a hop each dwell so a caller's retune stays honest, but the
    // channel never changes.
    const uint8_t one[1] = {6};
    ChannelHopper hopper(one, 1, kDwellMs);
    hopper.reset(0);
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
    TEST_ASSERT_TRUE(hopper.tick(300));
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
}

void test_reset_reparks_on_first_channel(void) {
    ChannelHopper hopper(kChannels, 3, kDwellMs);
    hopper.reset(0);
    hopper.tick(300);  // → channel 6
    hopper.tick(600);  // → channel 11
    hopper.reset(5000);
    TEST_ASSERT_EQUAL_UINT8(1, hopper.currentChannel());
    TEST_ASSERT_FALSE(hopper.tick(5001));           // dwell restarts from the reset time.
    TEST_ASSERT_TRUE(hopper.tick(5300));            // one dwell after reset: hop.
    TEST_ASSERT_EQUAL_UINT8(6, hopper.currentChannel());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_on_first_channel);
    RUN_TEST(test_holds_until_dwell_elapses_then_advances);
    RUN_TEST(test_sweeps_and_wraps);
    RUN_TEST(test_dwell_measured_from_last_hop_not_start);
    RUN_TEST(test_single_channel_sweep_hops_onto_itself);
    RUN_TEST(test_reset_reparks_on_first_channel);
    return UNITY_END();
}
