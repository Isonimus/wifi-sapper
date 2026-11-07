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
using sapper_test::buildPmkidM1;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kOther[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t kOtherClient[6] = {0x0A, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kPmkid[16] = {
    0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81,
    0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09};

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

// A clientless-PMKID capture (ADR-0110 #1): beacon + an M1 carrying a PMKID KDE, no M2. It is
// isPmkidValid (the engine's second terminal condition) but not isWpaSecValid (no M2 for the pcap).
void test_beacon_plus_pmkid_m1_is_pmkid_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildPmkidM1(kBssid, kClient, kPmkid));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));
    TEST_ASSERT_TRUE(c.isPmkidValid());
    TEST_ASSERT_FALSE(c.isWpaSecValid());  // no M2.
}

// A plain M1 with no PMKID KDE (the common 4-way M1) is not isPmkidValid even with a beacon.
void test_beacon_plus_plain_m1_is_not_pmkid_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));  // no KDE.
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));
    TEST_ASSERT_FALSE(c.isPmkidValid());
}

// A PMKID M1 with no beacon yet is not isPmkidValid (the ESSID is needed for the WPA*01 PMKID line).
void test_pmkid_m1_without_beacon_is_not_pmkid_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildPmkidM1(kBssid, kClient, kPmkid));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));
    TEST_ASSERT_FALSE(c.hasBeacon());
    TEST_ASSERT_FALSE(c.isPmkidValid());
}

// A full 4-way capture is isWpaSecValid and is not isPmkidValid (its M1 carries no PMKID KDE), so
// the two terminal conditions stay distinct.
void test_four_way_is_wpasec_valid_and_not_pmkid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false));
    TEST_ASSERT_TRUE(c.isWpaSecValid());
    TEST_ASSERT_FALSE(c.isPmkidValid());
}

// ADR-0115 pairing: a beacon + M1 + M2 whose M1 and M2 come from DIFFERENT stations is not
// wpa-sec-valid, even though all three messages are present. This is the solicited-PMKID / crowded-RF
// mispair (our M1 addressed to a spoofed STA, an ambient client's M2) that wpa-sec rejects. Presence
// alone passed before the fix (fail-before), the pairing check rejects it after (pass-after).
void test_mismatched_client_m1_m2_is_not_wpasec_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));       // M1 to kClient.
    ingestVec(c, buildEapol(kBssid, kOtherClient, sapper_test::kKeyInfoM2, false));  // M2 from another STA.
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M2));
    TEST_ASSERT_FALSE(c.isWpaSecValid());  // same BSSID, different stations: not one exchange.
}

// ADR-0115 pairing: a beacon + M1 + M2 from the SAME station but with MISMATCHED replay counters is
// not wpa-sec-valid (the ANonce and MIC belong to different handshakes, as measured on the field
// cards: MERCUSYS M1 rc=2 / M2 rc=3). Presence alone passed before the fix (fail-before).
void test_mismatched_replay_m1_m2_is_not_wpasec_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true, 0, /*replay=*/2));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false, 0, /*replay=*/3));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M2));
    TEST_ASSERT_FALSE(c.isWpaSecValid());  // same station, different exchange.
}

// ADR-0115 pairing: a genuine pair (same station, equal non-zero replay counter) is still
// wpa-sec-valid. Guards the fix against over-rejecting: the real deauth-forced capture must pass, and
// a non-zero counter proves the check compares the actual counters, not just the builder's default 0.
void test_matched_client_and_replay_is_wpasec_valid(void) {
    HandshakeCollector c(kBssid);
    ingestVec(c, buildBeacon(kBssid, "TargetNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true, 0, /*replay=*/7));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false, 0, /*replay=*/7));
    TEST_ASSERT_TRUE(c.isWpaSecValid());
}

void test_retarget_clears_and_aims_at_a_new_bssid(void) {
    // The endless hunt re-uses one collector across its round-robin, re-targeting between APs
    // (ADR-0015). A frame for the old target after retarget must be dropped, and one for the new
    // target collected.
    HandshakeCollector c(kBssid, 6);
    ingestVec(c, buildBeacon(kBssid, "FirstNet"));
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    TEST_ASSERT_TRUE(c.has(HandshakeMessage::M1));

    c.retarget(kOther, 11);
    TEST_ASSERT_FALSE(c.hasBeacon());
    TEST_ASSERT_FALSE(c.has(HandshakeMessage::M1));  // the old target's frames are gone.
    TEST_ASSERT_EQUAL_STRING("", c.handshake().ssid);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kOther, c.handshake().bssid, 6);
    TEST_ASSERT_EQUAL_UINT8(11, c.handshake().channel);

    // A frame for the previous target is now foreign and dropped; the new target's is collected.
    ingestVec(c, buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true));
    TEST_ASSERT_FALSE(c.has(HandshakeMessage::M1));
    ingestVec(c, buildBeacon(kOther, "SecondNet"));
    ingestVec(c, buildEapol(kOther, kClient, sapper_test::kKeyInfoM1, true));
    ingestVec(c, buildEapol(kOther, kClient, sapper_test::kKeyInfoM2, false));
    TEST_ASSERT_TRUE(c.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("SecondNet", c.handshake().ssid);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_collects_wpasec_valid_ignoring_foreign);
    RUN_TEST(test_stores_full_frame_bytes_unmodified);
    RUN_TEST(test_only_m1_is_not_wpasec_valid);
    RUN_TEST(test_full_four_way_is_complete);
    RUN_TEST(test_foreign_only_stays_empty);
    RUN_TEST(test_beacon_plus_pmkid_m1_is_pmkid_valid);
    RUN_TEST(test_beacon_plus_plain_m1_is_not_pmkid_valid);
    RUN_TEST(test_pmkid_m1_without_beacon_is_not_pmkid_valid);
    RUN_TEST(test_four_way_is_wpasec_valid_and_not_pmkid);
    RUN_TEST(test_mismatched_client_m1_m2_is_not_wpasec_valid);
    RUN_TEST(test_mismatched_replay_m1_m2_is_not_wpasec_valid);
    RUN_TEST(test_matched_client_and_replay_is_wpasec_valid);
    RUN_TEST(test_reset_clears_but_keeps_target);
    RUN_TEST(test_retarget_clears_and_aims_at_a_new_bssid);
    return UNITY_END();
}
