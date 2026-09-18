/**
 * @file test_ap_registry.cpp
 * @brief Native unit tests for the pure beacon-based AP registry (slice-0014 Scenario B, ADR-0013).
 *
 * Frames enter through onFrame() — the exact seam entry point the device's promiscuous callback calls
 * — so this proves discovery reaches the pure core through the real consumer, with no radio. It
 * covers distinct enumeration, first-sighting-wins deduplication, the DS-Parameter channel and its
 * sweep-channel fallback, non-beacon rejection, and bounded overflow.
 */
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "net/ap_registry.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;

static const uint8_t kApA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kApB[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t kClient[6] = {0x06, 0x01, 0x02, 0x03, 0x04, 0x05};

void setUp(void) {}
void tearDown(void) {}

static void deliver(FrameConsumer& consumer, const std::vector<uint8_t>& frame) {
    consumer.onFrame(frame.data(), static_cast<uint16_t>(frame.size()));
}

/// Index of the entry whose BSSID matches @p bssid, or -1 (order of discovery is not asserted).
static int findAp(const ApRegistry& reg, const uint8_t bssid[6]) {
    for (size_t i = 0; i < reg.count(); ++i) {
        if (std::memcmp(reg.at(i).bssid, bssid, 6) == 0) return static_cast<int>(i);
    }
    return -1;
}

void test_records_distinct_aps_with_ssid_and_ds_channel(void) {
    ApRegistry reg;
    deliver(reg, buildBeacon(kApA, "Alpha", /*channel=*/1));
    deliver(reg, buildBeacon(kApB, "Bravo", /*channel=*/11));

    TEST_ASSERT_EQUAL_UINT32(2, reg.count());
    const int a = findAp(reg, kApA);
    const int b = findAp(reg, kApB);
    TEST_ASSERT_TRUE(a >= 0 && b >= 0);
    TEST_ASSERT_EQUAL_STRING("Alpha", reg.at(a).ssid);
    TEST_ASSERT_EQUAL_UINT8(1, reg.at(a).channel);
    TEST_ASSERT_EQUAL_STRING("Bravo", reg.at(b).ssid);
    TEST_ASSERT_EQUAL_UINT8(11, reg.at(b).channel);
}

void test_duplicate_bssid_recorded_once_first_sighting_wins(void) {
    ApRegistry reg;
    deliver(reg, buildBeacon(kApA, "First", /*channel=*/6));
    // A second beacon for the same BSSID with different details must not add a row or overwrite.
    deliver(reg, buildBeacon(kApA, "Second", /*channel=*/9));

    TEST_ASSERT_EQUAL_UINT32(1, reg.count());
    TEST_ASSERT_EQUAL_STRING("First", reg.at(0).ssid);
    TEST_ASSERT_EQUAL_UINT8(6, reg.at(0).channel);
}

void test_channel_falls_back_to_sweep_channel_when_ds_param_absent(void) {
    ApRegistry reg;
    reg.setCurrentChannel(3);  // the sweep is parked on channel 3.
    deliver(reg, buildBeacon(kApA, "NoChan"));  // beacon with no DS Parameter Set IE.

    TEST_ASSERT_EQUAL_UINT32(1, reg.count());
    TEST_ASSERT_EQUAL_UINT8(3, reg.at(0).channel);  // fell back to the sweep channel.
}

void test_ds_param_channel_overrides_sweep_channel(void) {
    // The beacon's own channel is authoritative even when it differs from where we heard it (an
    // adjacent-channel bleed during a sweep).
    ApRegistry reg;
    reg.setCurrentChannel(1);
    deliver(reg, buildBeacon(kApA, "Real", /*channel=*/6));
    TEST_ASSERT_EQUAL_UINT8(6, reg.at(0).channel);
}

void test_out_of_band_ds_channel_falls_back_to_sweep_channel(void) {
    // A corrupt DS Parameter Set (out-of-band value) is treated as no channel, so the registry uses
    // the sweep-channel fallback rather than recording an impossible channel (ADR-0013).
    ApRegistry reg;
    reg.setCurrentChannel(6);
    deliver(reg, buildBeacon(kApA, "Corrupt", /*channel=*/200));
    TEST_ASSERT_EQUAL_UINT32(1, reg.count());
    TEST_ASSERT_EQUAL_UINT8(6, reg.at(0).channel);
}

void test_non_beacon_frame_is_ignored(void) {
    ApRegistry reg;
    deliver(reg, buildEapol(kApA, kClient, sapper_test::kKeyInfoM1, true));  // a data frame.
    TEST_ASSERT_EQUAL_UINT32(0, reg.count());
}

void test_hidden_ssid_ap_is_recorded_with_empty_name(void) {
    ApRegistry reg;
    deliver(reg, buildBeacon(kApA, "", /*channel=*/6));
    TEST_ASSERT_EQUAL_UINT32(1, reg.count());
    TEST_ASSERT_EQUAL_STRING("", reg.at(0).ssid);
    TEST_ASSERT_EQUAL_UINT8(6, reg.at(0).channel);
}

void test_overflow_is_reported_not_silently_dropped(void) {
    ApRegistry reg;
    // Fill the table exactly, then push one more distinct BSSID past it.
    for (size_t i = 0; i <= kMaxDiscoveredAps; ++i) {
        uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00,
                            static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i & 0xFF)};
        deliver(reg, buildBeacon(bssid, "Net", /*channel=*/1));
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxDiscoveredAps, reg.count());  // bounded, never grows past the cap.
    TEST_ASSERT_TRUE(reg.overflowed());                        // and the drop is surfaced, not hidden.
}

void test_reset_clears_the_enumeration(void) {
    ApRegistry reg;
    deliver(reg, buildBeacon(kApA, "Alpha", /*channel=*/1));
    TEST_ASSERT_EQUAL_UINT32(1, reg.count());
    reg.reset();
    TEST_ASSERT_EQUAL_UINT32(0, reg.count());
    TEST_ASSERT_FALSE(reg.overflowed());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_records_distinct_aps_with_ssid_and_ds_channel);
    RUN_TEST(test_duplicate_bssid_recorded_once_first_sighting_wins);
    RUN_TEST(test_channel_falls_back_to_sweep_channel_when_ds_param_absent);
    RUN_TEST(test_ds_param_channel_overrides_sweep_channel);
    RUN_TEST(test_out_of_band_ds_channel_falls_back_to_sweep_channel);
    RUN_TEST(test_non_beacon_frame_is_ignored);
    RUN_TEST(test_hidden_ssid_ap_is_recorded_with_empty_name);
    RUN_TEST(test_overflow_is_reported_not_silently_dropped);
    RUN_TEST(test_reset_clears_the_enumeration);
    return UNITY_END();
}
