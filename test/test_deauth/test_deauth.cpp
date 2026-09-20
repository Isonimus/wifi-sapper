/**
 * @file test_deauth.cpp
 * @brief Native unit tests for the pure deauth/disassoc frame builder and the TX seam contract
 *        (slice-0028 Scenarios A-F, ADR-0027).
 *
 * Proves the exact 802.11 byte layout, the deauth/disassoc subtype difference, little-endian reason
 * encoding, buffer-too-small refusal, destination selection, and that the built bytes reach a
 * RawTransmitter unchanged — the whole proof for this pure layer, no radio (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>

#include "net/deauth.h"

#include "../support/fake_raw_transmitter.h"

using namespace sapper;
using sapper_test::FakeRawTransmitter;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

void setUp(void) {}
void tearDown(void) {}

// Scenario A — the deauth frame has the exact 802.11 byte layout.
void test_deauth_byte_layout(void) {
    uint8_t frame[kDeauthFrameLen];
    const size_t n = buildDeauthFrame(frame, sizeof(frame), ManagementSubtype::Deauth, kClient,
                                      kBssid, DeauthReason::Unspecified);
    TEST_ASSERT_EQUAL_UINT(kDeauthFrameLen, n);

    TEST_ASSERT_EQUAL_UINT8(0xC0, frame[0]);  // FC: management + deauth subtype.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[1]);  // FC flags.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[2]);  // duration.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[3]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kClient, &frame[4], 6);   // Addr1: destination.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, &frame[10], 6);   // Addr2: source (spoofed AP).
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, &frame[16], 6);   // Addr3: BSSID.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[22]);  // sequence.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[23]);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame[24]);  // reason Unspecified=1, low byte.
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[25]);  // high byte.
}

// Scenario B — disassoc differs from deauth only in the subtype byte.
void test_disassoc_differs_only_in_subtype(void) {
    uint8_t deauth[kDeauthFrameLen];
    uint8_t disassoc[kDeauthFrameLen];
    buildDeauthFrame(deauth, sizeof(deauth), ManagementSubtype::Deauth, kClient, kBssid,
                     DeauthReason::DeauthLeaving);
    buildDeauthFrame(disassoc, sizeof(disassoc), ManagementSubtype::Disassoc, kClient, kBssid,
                     DeauthReason::DeauthLeaving);

    TEST_ASSERT_EQUAL_UINT8(0xC0, deauth[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA0, disassoc[0]);
    // Every byte after the subtype is identical.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&deauth[1], &disassoc[1], kDeauthFrameLen - 1);
}

// Scenario C — the reason code is written little-endian and is carried, not hard-coded.
void test_reason_is_little_endian(void) {
    uint8_t frame[kDeauthFrameLen];
    buildDeauthFrame(frame, sizeof(frame), ManagementSubtype::Deauth, kClient, kBssid,
                     DeauthReason::Class3FrameFromNonassoc);  // value 7.
    TEST_ASSERT_EQUAL_UINT8(0x07, frame[24]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame[25]);

    // A distinct reason produces distinct bytes there.
    uint8_t other[kDeauthFrameLen];
    buildDeauthFrame(other, sizeof(other), ManagementSubtype::Deauth, kClient, kBssid,
                     DeauthReason::DeauthLeaving);  // value 3.
    TEST_ASSERT_EQUAL_UINT8(0x03, other[24]);
}

// Scenario D — an undersized buffer is refused, not truncated.
void test_undersized_buffer_refused(void) {
    uint8_t small[kDeauthFrameLen - 1];
    std::memset(small, 0xEE, sizeof(small));
    const size_t n = buildDeauthFrame(small, sizeof(small), ManagementSubtype::Deauth, kClient,
                                      kBssid, DeauthReason::Unspecified);
    TEST_ASSERT_EQUAL_UINT(0, n);
    // Nothing was written: the sentinel bytes are untouched.
    for (size_t i = 0; i < sizeof(small); ++i) TEST_ASSERT_EQUAL_UINT8(0xEE, small[i]);
}

// Scenario E — the destination is exactly the caller's, broadcast or unicast.
void test_destination_is_caller_choice(void) {
    uint8_t bcast[kDeauthFrameLen];
    buildDeauthFrame(bcast, sizeof(bcast), ManagementSubtype::Deauth, kBroadcastMac, kBssid,
                     DeauthReason::Unspecified);
    const uint8_t allFf[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(allFf, &bcast[4], 6);
    // Source and BSSID stay the AP even for a broadcast destination.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, &bcast[10], 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBssid, &bcast[16], 6);

    uint8_t unicast[kDeauthFrameLen];
    buildDeauthFrame(unicast, sizeof(unicast), ManagementSubtype::Deauth, kClient, kBssid,
                     DeauthReason::Unspecified);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kClient, &unicast[4], 6);
}

// Scenario F — the builder's bytes reach the transmitter seam unchanged.
void test_built_bytes_reach_transmitter(void) {
    uint8_t frame[kDeauthFrameLen];
    const size_t n = buildDeauthFrame(frame, sizeof(frame), ManagementSubtype::Disassoc, kClient,
                                      kBssid, DeauthReason::Class3FrameFromNonassoc);

    FakeRawTransmitter tx;
    const bool ok = tx.transmit(frame, static_cast<uint16_t>(n));

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(1, tx.count());
    TEST_ASSERT_EQUAL_UINT(kDeauthFrameLen, tx.last().size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, tx.last().data(), kDeauthFrameLen);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_deauth_byte_layout);
    RUN_TEST(test_disassoc_differs_only_in_subtype);
    RUN_TEST(test_reason_is_little_endian);
    RUN_TEST(test_undersized_buffer_refused);
    RUN_TEST(test_destination_is_caller_choice);
    RUN_TEST(test_built_bytes_reach_transmitter);
    return UNITY_END();
}
