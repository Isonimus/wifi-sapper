/**
 * @file test_channel_list_arg.cpp
 * @brief Native unit tests for the shared channel-list argument parser (ADR-0013/ADR-0015).
 *
 * The parser is shared by the hop-taking bench probes (rf_discover_probe, hunt_probe); a malformed
 * list must fail loud, never sweep a partial or wrong band (§3). Pure and host-tested, so both probes
 * inherit one proven parse.
 */
#include <unity.h>

#include "net/channel_list_arg.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

void test_parses_a_valid_list(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_TRUE(parseChannelListArg("1,6,11", out, count));
    TEST_ASSERT_EQUAL_UINT32(3, count);
    TEST_ASSERT_EQUAL_UINT8(1, out[0]);
    TEST_ASSERT_EQUAL_UINT8(6, out[1]);
    TEST_ASSERT_EQUAL_UINT8(11, out[2]);
}

void test_parses_a_single_channel(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_TRUE(parseChannelListArg("6", out, count));
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_UINT8(6, out[0]);
}

void test_band_edges_are_valid(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_TRUE(parseChannelListArg("1,14", out, count));
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_UINT8(1, out[0]);
    TEST_ASSERT_EQUAL_UINT8(14, out[1]);
}

void test_rejects_empty_and_null(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_FALSE(parseChannelListArg("", out, count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
    TEST_ASSERT_FALSE(parseChannelListArg(nullptr, out, count));
}

void test_rejects_out_of_band(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_FALSE(parseChannelListArg("15", out, count));   // above the 2.4 GHz ceiling.
    TEST_ASSERT_FALSE(parseChannelListArg("0", out, count));    // below the floor.
    TEST_ASSERT_FALSE(parseChannelListArg("1,99", out, count)); // one bad token spoils the list.
}

void test_rejects_malformed_separators(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    TEST_ASSERT_FALSE(parseChannelListArg("abc", out, count));  // not a number.
    TEST_ASSERT_FALSE(parseChannelListArg("1,6,", out, count)); // trailing comma.
    TEST_ASSERT_FALSE(parseChannelListArg("1;6", out, count));  // separator other than a comma.
    TEST_ASSERT_FALSE(parseChannelListArg(",1", out, count));   // leading comma.
}

void test_rejects_more_channels_than_a_sweep_holds(void) {
    uint8_t out[kMaxHopChannels] = {0};
    size_t count = 0;
    // kMaxHopChannels + 1 valid tokens: the parser stops rather than overrun the fixed array.
    TEST_ASSERT_FALSE(parseChannelListArg("1,1,1,1,1,1,1,1,1,1,1,1,1,1,1", out, count));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parses_a_valid_list);
    RUN_TEST(test_parses_a_single_channel);
    RUN_TEST(test_band_edges_are_valid);
    RUN_TEST(test_rejects_empty_and_null);
    RUN_TEST(test_rejects_out_of_band);
    RUN_TEST(test_rejects_malformed_separators);
    RUN_TEST(test_rejects_more_channels_than_a_sweep_holds);
    return UNITY_END();
}
