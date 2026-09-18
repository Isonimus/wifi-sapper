/**
 * @file test_handshake_collector.cpp
 * @brief Native unit tests for the handshake accumulator (slice-0010 Scenario B, ADR-0009).
 *
 * Proves target matching, wpa-sec/complete gating, foreign-BSSID rejection, byte fidelity, and
 * reset against crafted frames — no device needed (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>
#include <vector>

#include "net/handshake_collector.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kOther[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

void setUp(void) {}
void tearDown(void) {}

static void ingestVec(HandshakeCollector& c, const std::vector<uint8_t>& f) {
    c.ingest(f.data(), static_cast<uint16_t>(f.size()));
}

void test_collects_wpasec_valid_ignoring_foreign(void) {
    HandshakeCollector c(kBssid, 6);
    // Interleave a foreign network's beacon+M1 — they must be ignored entirely.
    ingestVec(c, buildBeacon(kOther, "NeighborNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false));  // out-of-order M2.
    ingestVec(c, buildEapol(kOther, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));

    TEST_ASSERT_TRUE(c.isWpaSecValid());
    TEST_ASSERT_FALSE(c.isComplete());  // no M3/M4.
    TEST_ASSERT_EQUAL_STRING("TargetNet", c.handshake().ssid);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, c.handshake().bssid, 6);
    TEST_ASSERT_EQUAL_UINT8(6, c.handshake().channel);
}

void test_stores_full_frame_bytes_unmodified(void) {
    HandshakeCollector c(kBssid);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    ingestVec(c, beacon);
    ingestVec(c, m1);

    TEST_ASSERT_EQUAL_UINT16(beacon.size(), c.handshake().beacon.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(beacon.data(), c.handshake().beacon.data, beacon.size());
    TEST_ASSERT_EQUAL_UINT16(m1.size(), c.handshake().msg[0].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(m1.data(), c.handshake().msg[0].data, m1.size());
}

void test_only_m1_is_not_wpasec_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));
    TEST_ASSERT_FALSE(c.hasBeacon());
    TEST_ASSERT_FALSE(c.isWpaSecValid());
}

void test_full_four_way_is_complete(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM3, true));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM4, false));
    TEST_ASSERT_TRUE(c.isComplete());
    TEST_ASSERT_TRUE(c.isWpaSecValid());
}

void test_foreign_only_stays_empty(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kOther, "NeighborNet"));
    ingestVec(c, buildEapol(kOther, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kOther, kClient, sapper_test::kKeyInfoM2, false));
    TEST_ASSERT_FALSE(c.hasBeacon());
    TEST_ASSERT_FALSE(c.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("", c.handshake().ssid);
}

void test_reset_clears_but_keeps_target(void) {
    HandshakeCollector c(kBssid, 11);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false));
    TEST_ASSERT_TRUE(c.isWpaSecValid());

    c.reset();
    TEST_ASSERT_FALSE(c.hasBeacon());
    TEST_ASSERT_FALSE(c.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("", c.handshake().ssid);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, c.handshake().bssid, 6);  // target retained.
    TEST_ASSERT_EQUAL_UINT8(11, c.handshake().channel);

    // The same collector still works for the retained target after reset.
    ingestVec(c, buildBeacon(kBssid, "AgainNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false));
    TEST_ASSERT_TRUE(c.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("AgainNet", c.handshake().ssid);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_collects_wpasec_valid_ignoring_foreign);
    RUN_TEST(test_stores_full_frame_bytes_unmodified);
    RUN_TEST(test_only_m1_is_not_wpasec_valid);
    RUN_TEST(test_full_four_way_is_complete);
    RUN_TEST(test_foreign_only_stays_empty);
    RUN_TEST(test_reset_clears_but_keeps_target);
    return UNITY_END();
}
