/**
 * @file test_radio_sniffer.cpp
 * @brief Native unit tests for the RX seam wiring (slice-0012 Scenario A, ADR-0011).
 *
 * The abstract RadioSniffer/FrameConsumer seam has no logic to test; what this proves is that the
 * one concrete consumer — HandshakeConsumer — forwards frames delivered through onFrame() (the exact
 * entry point the device's promiscuous callback calls) into the pure HandshakeCollector. The radio
 * itself is device-only and proved on air by the verify script (Scenario B).
 */
#include <unity.h>

#include <cstring>
#include <vector>

#include "net/handshake_collector.h"
#include "net/handshake_consumer.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kOtherBssid[6] = {0x0A, 0x99, 0x88, 0x77, 0x66, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

void setUp(void) {}
void tearDown(void) {}

// Deliver a frame the way the radio would: through the FrameConsumer seam, not by calling ingest().
static void deliver(FrameConsumer& consumer, const std::vector<uint8_t>& frame) {
    consumer.onFrame(frame.data(), static_cast<uint16_t>(frame.size()));
}

void test_onframe_delivers_frames_to_collector(void) {
    HandshakeCollector collector(kBssid);
    HandshakeConsumer consumer(collector);

    std::vector<uint8_t> beacon = buildBeacon(kBssid, "SeamNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    // A foreign frame interleaved: it must not leak into this target's collector.
    std::vector<uint8_t> foreign = buildBeacon(kOtherBssid, "NotOurs");

    deliver(consumer, beacon);
    deliver(consumer, foreign);
    deliver(consumer, m1);
    deliver(consumer, m2);

    TEST_ASSERT_TRUE(collector.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("SeamNet", collector.handshake().ssid);
    // The seam forwarded the frame pointer and length in order: the stored beacon is byte-identical.
    const CapturedFrame& storedBeacon = collector.handshake().beacon;
    TEST_ASSERT_EQUAL_UINT16(beacon.size(), storedBeacon.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(beacon.data(), storedBeacon.data, beacon.size());
}

void test_onframe_ignores_foreign_only(void) {
    HandshakeCollector collector(kBssid);
    HandshakeConsumer consumer(collector);

    deliver(consumer, buildBeacon(kOtherBssid, "NotOurs"));
    deliver(consumer, buildEapol(kOtherBssid, kClient, sapper_test::kKeyInfoM1, true));

    TEST_ASSERT_FALSE(collector.hasBeacon());
    TEST_ASSERT_FALSE(collector.isWpaSecValid());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_onframe_delivers_frames_to_collector);
    RUN_TEST(test_onframe_ignores_foreign_only);
    return UNITY_END();
}
