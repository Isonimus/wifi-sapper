/**
 * @file test_pcap.cpp
 * @brief Native unit tests for pcap serialization (slice-0010 Scenario C, ADR-0009).
 *
 * Proves the global-header fields, record count/order, per-record length fidelity, byte fidelity,
 * and the fail-loud refusals (not-valid handshake; failing sink) — no device needed (§3).
 */
#include <unity.h>

#include <cstring>
#include <vector>

#include "net/handshake_collector.h"
#include "net/pcap.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;
using sapper_test::buildPmkidM1;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t kPmkid[16] = {
    0x1a, 0x2b, 0x3c, 0x4d, 0x5e, 0x6f, 0x70, 0x81,
    0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x09};

constexpr size_t kGlobalHeaderLen = 24;
constexpr size_t kRecordHeaderLen = 16;

void setUp(void) {}
void tearDown(void) {}

/// Collects every byte written; the host stand-in for the device SD/LittleFS/upload sink.
class BufferSink : public CaptureSink {
public:
    bool write(const uint8_t* data, size_t len) override {
        bytes.insert(bytes.end(), data, data + len);
        return true;
    }
    std::vector<uint8_t> bytes;
};

/// Fails the Nth write (1-based), so a partial-write failure can be forced mid-stream.
class FailingSink : public CaptureSink {
public:
    explicit FailingSink(int failOn) : failOn_(failOn) {}
    bool write(const uint8_t*, size_t) override { return ++calls_ != failOn_; }

private:
    int failOn_;
    int calls_ = 0;
};

static uint32_t readU32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    std::memcpy(&v, &b[off], sizeof(v));
    return v;
}
static uint16_t readU16(const std::vector<uint8_t>& b, size_t off) {
    uint16_t v = 0;
    std::memcpy(&v, &b[off], sizeof(v));
    return v;
}

void test_global_header_fields(void) {
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    c.ingest(beacon.data(), beacon.size());
    c.ingest(m1.data(), m1.size());
    c.ingest(m2.data(), m2.size());

    BufferSink sink;
    TEST_ASSERT_TRUE(serializeHandshake(c.handshake(), sink));
    TEST_ASSERT_GREATER_THAN(kGlobalHeaderLen, sink.bytes.size());
    TEST_ASSERT_EQUAL_HEX32(kPcapMagicMicroseconds, readU32(sink.bytes, 0));
    TEST_ASSERT_EQUAL_UINT16(kPcapVersionMajor, readU16(sink.bytes, 4));
    TEST_ASSERT_EQUAL_UINT16(kPcapVersionMinor, readU16(sink.bytes, 6));
    TEST_ASSERT_EQUAL_UINT32(kPcapSnapLen, readU32(sink.bytes, 16));
    TEST_ASSERT_EQUAL_UINT32(kPcapLinkTypeIeee80211, readU32(sink.bytes, 20));
}

// Walk the records after the global header and assert each matches an expected frame in order.
static void assertRecords(const std::vector<uint8_t>& b,
                          const std::vector<std::vector<uint8_t>>& expected) {
    size_t cursor = kGlobalHeaderLen;
    for (const std::vector<uint8_t>& frame : expected) {
        TEST_ASSERT_TRUE(cursor + kRecordHeaderLen <= b.size());
        const uint32_t inclLen = readU32(b, cursor + 8);
        const uint32_t origLen = readU32(b, cursor + 12);
        TEST_ASSERT_EQUAL_UINT32(frame.size(), inclLen);
        TEST_ASSERT_EQUAL_UINT32(frame.size(), origLen);
        cursor += kRecordHeaderLen;
        TEST_ASSERT_TRUE(cursor + frame.size() <= b.size());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(frame.data(), &b[cursor], frame.size());
        cursor += frame.size();
    }
    TEST_ASSERT_EQUAL_UINT32(b.size(), cursor);  // no trailing bytes: exactly these records.
}

void test_beacon_m1_m2_records_in_order(void) {
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    c.ingest(beacon.data(), beacon.size());
    c.ingest(m1.data(), m1.size());
    c.ingest(m2.data(), m2.size());

    BufferSink sink;
    TEST_ASSERT_TRUE(serializeHandshake(c.handshake(), sink));
    assertRecords(sink.bytes, {beacon, m1, m2});
}

void test_full_set_emits_five_records(void) {
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    std::vector<uint8_t> m3 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM3, true);
    std::vector<uint8_t> m4 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM4, false);
    for (const std::vector<uint8_t>* f : {&beacon, &m1, &m2, &m3, &m4}) c.ingest(f->data(), f->size());

    BufferSink sink;
    TEST_ASSERT_TRUE(serializeHandshake(c.handshake(), sink));
    assertRecords(sink.bytes, {beacon, m1, m2, m3, m4});
}

void test_refuses_not_wpasec_valid(void) {
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    c.ingest(m1.data(), m1.size());  // M1 only: not uploadable.

    BufferSink sink;
    TEST_ASSERT_FALSE(serializeHandshake(c.handshake(), sink));
    TEST_ASSERT_EQUAL_UINT32(0, sink.bytes.size());  // wrote nothing.
}

void test_serializes_pmkid_capture_beacon_plus_m1(void) {
    // A clientless-PMKID capture (beacon + an M1 carrying a PMKID KDE, no M2) is uploadable (ADR-0110 #1):
    // serializeHandshake emits the {beacon + M1} pcap, two records. Fail-before: the isWpaSecValid-only
    // gate returned false for a capture with no M2, dropping every PMKID at the queue (a lost handshake).
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildPmkidM1(kBssid, kClient, kPmkid);
    c.ingest(beacon.data(), beacon.size());
    c.ingest(m1.data(), m1.size());
    TEST_ASSERT_TRUE(c.handshake().isPmkidValid());
    TEST_ASSERT_FALSE(c.handshake().isWpaSecValid());  // no M2.

    BufferSink sink;
    TEST_ASSERT_TRUE(serializeHandshake(c.handshake(), sink));
    const size_t expected = kGlobalHeaderLen + 2 * kRecordHeaderLen + beacon.size() + m1.size();
    TEST_ASSERT_EQUAL_UINT32(expected, sink.bytes.size());  // global header + beacon record + M1 record.
}

void test_refuses_beacon_without_m1(void) {
    // A lone beacon is neither wpa-sec-valid nor PMKID-valid (no M1): serialize nothing, like a lone M1.
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    c.ingest(beacon.data(), beacon.size());

    BufferSink sink;
    TEST_ASSERT_FALSE(serializeHandshake(c.handshake(), sink));
    TEST_ASSERT_EQUAL_UINT32(0, sink.bytes.size());
}

void test_failing_sink_propagates_failure(void) {
    HandshakeCollector c(kBssid, 6);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    c.ingest(beacon.data(), beacon.size());
    c.ingest(m1.data(), m1.size());
    c.ingest(m2.data(), m2.size());

    FailingSink onHeader(1);  // fails the global-header write.
    TEST_ASSERT_FALSE(serializeHandshake(c.handshake(), onHeader));

    FailingSink midStream(3);  // fails partway through the records.
    TEST_ASSERT_FALSE(serializeHandshake(c.handshake(), midStream));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_global_header_fields);
    RUN_TEST(test_beacon_m1_m2_records_in_order);
    RUN_TEST(test_full_set_emits_five_records);
    RUN_TEST(test_refuses_not_wpasec_valid);
    RUN_TEST(test_serializes_pmkid_capture_beacon_plus_m1);
    RUN_TEST(test_refuses_beacon_without_m1);
    RUN_TEST(test_failing_sink_propagates_failure);
    return UNITY_END();
}
