/**
 * @file test_eapol.cpp
 * @brief Native unit tests for the pure 802.11/EAPOL frame readers (slice-0010 Scenario A, ADR-0009).
 *
 * Proves message identification, EAPOL location, BSSID extraction, and beacon-SSID reading against
 * crafted frames — the whole proof for this pure layer, no device needed (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>
#include <vector>

#include "net/eapol.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

void setUp(void) {}
void tearDown(void) {}

// Identify each message from a frame built with its Key Information flags.
static HandshakeMessage identifyFrame(const std::vector<uint8_t>& frame) {
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame.data(), static_cast<uint16_t>(frame.size()), eapolLen);
    TEST_ASSERT_NOT_NULL(eapol);
    return identifyMessage(eapol, eapolLen);
}

void test_identifies_each_pairwise_message(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::M1),
                      static_cast<int>(identifyFrame(buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true))));
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::M2),
                      static_cast<int>(identifyFrame(buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false))));
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::M3),
                      static_cast<int>(identifyFrame(buildEapol(kBssid, kClient, sapper_test::kKeyInfoM3, true))));
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::M4),
                      static_cast<int>(identifyFrame(buildEapol(kBssid, kClient, sapper_test::kKeyInfoM4, false))));
}

void test_group_key_frame_is_unknown(void) {
    // Ack+MIC+Secure but NOT pairwise: a group-key rekey, not a 4-way message.
    const uint16_t groupKey = sapper_test::kKiAck | sapper_test::kKiMic | sapper_test::kKiSecure;
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::Unknown),
                      static_cast<int>(identifyFrame(buildEapol(kBssid, kClient, groupKey, true))));
}

void test_non_key_eapol_type_is_unknown(void) {
    std::vector<uint8_t> frame = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame.data(), static_cast<uint16_t>(frame.size()), eapolLen);
    TEST_ASSERT_NOT_NULL(eapol);
    // Corrupt the EAPOL type (offset 1) to EAPOL-Start (0x01): no longer an EAPOL-Key frame.
    const_cast<uint8_t*>(eapol)[1] = 0x01;
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::Unknown),
                      static_cast<int>(identifyMessage(eapol, eapolLen)));
}

void test_too_short_eapol_is_unknown(void) {
    std::vector<uint8_t> frame = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame.data(), static_cast<uint16_t>(frame.size()), eapolLen);
    TEST_ASSERT_NOT_NULL(eapol);
    TEST_ASSERT_EQUAL(static_cast<int>(HandshakeMessage::Unknown),
                      static_cast<int>(identifyMessage(eapol, 50)));  // under the 99-byte minimum.
}

void test_locate_eapol_absent_without_snap_header(void) {
    // A beacon carries no LLC/SNAP EAPOL header.
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TestNet");
    uint16_t eapolLen = 123;
    const uint8_t* eapol = locateEapol(beacon.data(), static_cast<uint16_t>(beacon.size()), eapolLen);
    TEST_ASSERT_NULL(eapol);
    TEST_ASSERT_EQUAL_UINT16(0, eapolLen);
}

void test_frame_bssid_by_ds_bits(void) {
    // AP→client (From-DS): BSSID in Addr2.
    std::vector<uint8_t> fromAp = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, frameBssid(fromAp.data(), fromAp.size()), 6);
    // client→AP (To-DS): BSSID in Addr1.
    std::vector<uint8_t> toAp = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, frameBssid(toAp.data(), toAp.size()), 6);
    // management/beacon (To-DS=From-DS=0): BSSID in Addr3.
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TestNet");
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, frameBssid(beacon.data(), beacon.size()), 6);
}

void test_beacon_ssid_read(void) {
    char ssid[33];
    std::vector<uint8_t> named = buildBeacon(kBssid, "MyNetwork");
    TEST_ASSERT_TRUE(beaconSsid(named.data(), named.size(), ssid));
    TEST_ASSERT_EQUAL_STRING("MyNetwork", ssid);
}

void test_hidden_beacon_ssid_is_empty_but_present(void) {
    char ssid[33];
    std::vector<uint8_t> hidden = buildBeacon(kBssid, "");
    TEST_ASSERT_TRUE(beaconSsid(hidden.data(), hidden.size(), ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_oversize_ssid_ie_is_rejected(void) {
    // An SSID element claiming more than the 32-octet 802.11 maximum is itself malformed. The
    // contract is "malformed → false", not silently clamp to 32 and report success (fail loud).
    char ssid[33];
    std::vector<uint8_t> frame = buildBeacon(kBssid, std::string(40, 'A'));
    TEST_ASSERT_FALSE(beaconSsid(frame.data(), frame.size(), ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_beacon_without_ssid_ie_reports_absent(void) {
    // Header + fixed body only, no IE list.
    std::vector<uint8_t> frame(24 + 12, 0);
    frame[0] = 0x80;
    char ssid[33];
    TEST_ASSERT_FALSE(beaconSsid(frame.data(), frame.size(), ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_is_beacon(void) {
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TestNet");
    std::vector<uint8_t> data = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    TEST_ASSERT_TRUE(isBeacon(beacon.data(), beacon.size()));
    TEST_ASSERT_FALSE(isBeacon(data.data(), data.size()));
}

void test_beacon_channel_from_ds_param(void) {
    // The DS Parameter Set IE (appended after the SSID) is the AP's own channel statement.
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "Named", /*channel=*/11);
    TEST_ASSERT_EQUAL_UINT8(11, beaconChannel(beacon.data(), beacon.size()));
    // The SSID must still read with the DS Parameter Set IE present after it (walk regression).
    char ssid[33];
    TEST_ASSERT_TRUE(beaconSsid(beacon.data(), beacon.size(), ssid));
    TEST_ASSERT_EQUAL_STRING("Named", ssid);
}

void test_beacon_channel_absent_is_zero(void) {
    // No DS Parameter Set IE → 0 (unknown), never a guess (ADR-0013).
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "NoChan");
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(beacon.data(), beacon.size()));
}

void test_beacon_channel_malformed_ie_list_is_zero(void) {
    // An IE claiming more bytes than the frame holds is a malformed list; the walk stops and reports
    // unknown rather than reading past the frame end.
    std::vector<uint8_t> frame(24 + 12, 0);
    frame[0] = 0x80;
    frame.push_back(0x03);  // DS Parameter Set id...
    frame.push_back(0x05);  // ...claiming 5 octets...
    frame.push_back(0x06);  // ...but only one is present.
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(frame.data(), frame.size()));
}

void test_beacon_channel_out_of_band_is_zero(void) {
    // A well-formed one-octet DS Parameter Set carrying an out-of-band value (a corrupt or spoofed
    // element) is not a channel the radio can tune to: report unknown, never the raw octet (ADR-0013).
    std::vector<uint8_t> high = buildBeacon(kBssid, "Corrupt", /*channel=*/200);
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(high.data(), high.size()));
    std::vector<uint8_t> zero = buildBeacon(kBssid, "Corrupt", /*channel=*/0);
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(zero.data(), zero.size()));
    std::vector<uint8_t> justOver = buildBeacon(kBssid, "Corrupt", /*channel=*/15);
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(justOver.data(), justOver.size()));
    // The band edges (1 and 14) are valid and must still read through.
    std::vector<uint8_t> low = buildBeacon(kBssid, "Edge", /*channel=*/1);
    TEST_ASSERT_EQUAL_UINT8(1, beaconChannel(low.data(), low.size()));
    std::vector<uint8_t> top = buildBeacon(kBssid, "Edge", /*channel=*/14);
    TEST_ASSERT_EQUAL_UINT8(14, beaconChannel(top.data(), top.size()));
}

void test_beacon_channel_wrong_length_is_zero(void) {
    // A well-formed DS Parameter Set is exactly one octet; a two-octet element is malformed → 0.
    std::vector<uint8_t> frame(24 + 12, 0);
    frame[0] = 0x80;
    frame.push_back(0x03);  // DS Parameter Set id
    frame.push_back(0x02);  // length 2 (malformed)
    frame.push_back(0x06);
    frame.push_back(0x00);
    TEST_ASSERT_EQUAL_UINT8(0, beaconChannel(frame.data(), frame.size()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_identifies_each_pairwise_message);
    RUN_TEST(test_group_key_frame_is_unknown);
    RUN_TEST(test_non_key_eapol_type_is_unknown);
    RUN_TEST(test_too_short_eapol_is_unknown);
    RUN_TEST(test_locate_eapol_absent_without_snap_header);
    RUN_TEST(test_frame_bssid_by_ds_bits);
    RUN_TEST(test_beacon_ssid_read);
    RUN_TEST(test_hidden_beacon_ssid_is_empty_but_present);
    RUN_TEST(test_oversize_ssid_ie_is_rejected);
    RUN_TEST(test_beacon_without_ssid_ie_reports_absent);
    RUN_TEST(test_is_beacon);
    RUN_TEST(test_beacon_channel_from_ds_param);
    RUN_TEST(test_beacon_channel_absent_is_zero);
    RUN_TEST(test_beacon_channel_out_of_band_is_zero);
    RUN_TEST(test_beacon_channel_malformed_ie_list_is_zero);
    RUN_TEST(test_beacon_channel_wrong_length_is_zero);
    return UNITY_END();
}
