/**
 * @file test_pcap_reader.cpp
 * @brief Native unit tests for pcap-to-CapturedHandshake parsing (ADR-0109 decision 2, §4 #46a).
 *
 * The drift guard for the re-derive-at-drain design: a handshake serialized by the real serializeHandshake
 * (net/pcap.cpp) and parsed back by parseHandshakePcap must reproduce the same frames, BSSID and SSID, and
 * must yield the *identical* hc22000 line as building directly from the live handshake (so the pwncrack feed
 * derived from the persisted pcap equals the feed we would have built at capture time). The fail-loud half
 * proves a truncated / wrong-magic / wrong-link-type / over-long / incomplete pcap returns false with no
 * partial handshake. No device needed (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "net/eapol.h"
#include "net/handshake_collector.h"
#include "net/hc22000.h"
#include "net/pcap.h"
#include "net/pcap_reader.h"

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

// The collector's watched channel is NOT carried by the pcap; the beacon's DS-param channel is the only
// channel the pcap holds. They are deliberately kept different so the channel round-trip assertion proves
// parsed.channel is beacon-derived, not coincidentally equal to the watched channel (review LOW #3).
static constexpr uint8_t kWatchedChannel = 6;
static constexpr uint8_t kBeaconDsChannel = 11;

// EAPOL field offsets from the EAPOL header start (mirrors hc22000.cpp / ADR-0108), for poking nonces/MIC.
static constexpr uint16_t kNonceOffset = 17;
static constexpr uint16_t kNonceLen = 32;
static constexpr uint16_t kMicOffset = 81;
static constexpr uint16_t kMicLen = 16;

void setUp(void) {}
void tearDown(void) {}

/// Collects every serialized byte; the host stand-in for the device upload buffer (as in test_pcap).
class BufferSink : public CaptureSink {
public:
    bool write(const uint8_t* data, size_t len) override {
        bytes.insert(bytes.end(), data, data + len);
        return true;
    }
    std::vector<uint8_t> bytes;
};

// Poke `n` bytes into the EAPOL payload of `frame` at `eapolOffset`, in place (before ingest, so the
// collector stores the poked frame). Mirrors test_hc22000's helper.
static void pokeEapol(std::vector<uint8_t>& frame, uint16_t eapolOffset, const uint8_t* src, size_t n) {
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame.data(), static_cast<uint16_t>(frame.size()), eapolLen);
    TEST_ASSERT_NOT_NULL(eapol);
    TEST_ASSERT_TRUE(eapolOffset + n <= eapolLen);
    size_t base = static_cast<size_t>(eapol - frame.data()) + eapolOffset;
    std::memcpy(frame.data() + base, src, n);
}

// Build a realistic 4-way handshake (beacon + M1 carrying anonce + M2 carrying snonce + mic), optionally
// with M3/M4, via the real collector so the stored frames and identity match the production path exactly.
static CapturedHandshake makeHandshake(const char* ssid, bool withM3M4 = false) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) { anonce[i] = static_cast<uint8_t>(0x01 + i); snonce[i] = static_cast<uint8_t>(0x41 + i); }
    for (int i = 0; i < 16; ++i) mic[i] = static_cast<uint8_t>(0xB0 + i);

    HandshakeCollector c(kBssid, kWatchedChannel);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, ssid, /*channel=*/kBeaconDsChannel);
    c.ingest(beacon.data(), beacon.size());

    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    pokeEapol(m1, kNonceOffset, anonce, kNonceLen);
    c.ingest(m1.data(), m1.size());

    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false);
    pokeEapol(m2, kNonceOffset, snonce, kNonceLen);
    pokeEapol(m2, kMicOffset, mic, kMicLen);
    c.ingest(m2.data(), m2.size());

    if (withM3M4) {
        std::vector<uint8_t> m3 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM3, true);
        std::vector<uint8_t> m4 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM4, false);
        c.ingest(m3.data(), m3.size());
        c.ingest(m4.data(), m4.size());
    }
    return c.handshake();
}

// Build a clientless-PMKID capture (beacon + an M1 carrying a PMKID KDE, no M2) via the real collector,
// so the stored frames and identity match the production path (ADR-0110).
static CapturedHandshake makePmkidHandshake(const char* ssid) {
    HandshakeCollector c(kBssid, kWatchedChannel);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, ssid, /*channel=*/kBeaconDsChannel);
    c.ingest(beacon.data(), beacon.size());
    std::vector<uint8_t> m1 = buildPmkidM1(kBssid, kClient, kPmkid);
    c.ingest(m1.data(), m1.size());
    return c.handshake();
}

static std::vector<uint8_t> serialize(const CapturedHandshake& hs) {
    BufferSink sink;
    TEST_ASSERT_TRUE(serializeHandshake(hs, sink));
    return sink.bytes;
}

static void assertFrameEqual(const CapturedFrame& a, const CapturedFrame& b) {
    TEST_ASSERT_EQUAL_UINT16(a.len, b.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a.data, b.data, a.len);
}

// --- Round trip: the serialized handshake parses back to the same frames/BSSID/SSID. ---
void test_round_trip_reproduces_frames_bssid_ssid(void) {
    CapturedHandshake original = makeHandshake("TargetNet");
    std::vector<uint8_t> bytes = serialize(original);

    CapturedHandshake parsed;
    TEST_ASSERT_TRUE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));

    TEST_ASSERT_TRUE(parsed.isWpaSecValid());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original.bssid, parsed.bssid, 6);
    TEST_ASSERT_EQUAL_STRING(original.ssid, parsed.ssid);
    // channel is recovered from the beacon DS-param IE, not the collector's watched channel: the pcap
    // carries no watched-channel field, so parsed.channel is the AP's advertised channel, deliberately
    // different from the watched channel the live handshake kept (review LOW #3).
    TEST_ASSERT_EQUAL_UINT8(kBeaconDsChannel, parsed.channel);
    TEST_ASSERT_EQUAL_UINT8(kWatchedChannel, original.channel);
    assertFrameEqual(original.beacon, parsed.beacon);
    assertFrameEqual(original.msg[0], parsed.msg[0]);
    assertFrameEqual(original.msg[1], parsed.msg[1]);
}

// --- The decisive guarantee: the hc22000 line from the parsed handshake equals the direct one. ---
void test_hc22000_from_pcap_equals_hc22000_from_handshake(void) {
    CapturedHandshake original = makeHandshake("TargetNet");
    std::vector<uint8_t> bytes = serialize(original);

    CapturedHandshake parsed;
    TEST_ASSERT_TRUE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));

    char direct[kHc22000LineMax];
    char viaPcap[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(original, direct, sizeof(direct)));
    TEST_ASSERT_TRUE(buildHc22000Line(parsed, viaPcap, sizeof(viaPcap)));
    TEST_ASSERT_EQUAL_STRING(direct, viaPcap);
}

// --- A clientless-PMKID capture round-trips, and its WPA*02 line from the pcap equals the direct one. ---
void test_pmkid_round_trips_and_wpa02_from_pcap_equals_direct(void) {
    // The §4 #46a round-trip extended to the PMKID path (ADR-0110 #5e): a {beacon + M1} PMKID capture
    // serialized and parsed back must reproduce the frames, stay isPmkidValid, and yield the identical
    // WPA*02 line whether built from the original handshake or the pcap-parsed one (what the drain does).
    CapturedHandshake original = makePmkidHandshake("TargetNet");
    TEST_ASSERT_TRUE(original.isPmkidValid());
    std::vector<uint8_t> bytes = serialize(original);

    CapturedHandshake parsed;
    TEST_ASSERT_TRUE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
    TEST_ASSERT_TRUE(parsed.isPmkidValid());
    TEST_ASSERT_FALSE(parsed.isWpaSecValid());  // no M2 survived (there was none).
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original.bssid, parsed.bssid, 6);
    TEST_ASSERT_EQUAL_STRING(original.ssid, parsed.ssid);
    assertFrameEqual(original.beacon, parsed.beacon);
    assertFrameEqual(original.msg[0], parsed.msg[0]);

    char direct[kHc22000LineMax];
    char viaPcap[kHc22000LineMax];
    TEST_ASSERT_FALSE(buildHc22000Line(parsed, viaPcap, sizeof(viaPcap)));  // no WPA*01 (no 4-way).
    TEST_ASSERT_TRUE(buildPmkidHc22000Line(original, direct, sizeof(direct)));
    TEST_ASSERT_TRUE(buildPmkidHc22000Line(parsed, viaPcap, sizeof(viaPcap)));
    TEST_ASSERT_EQUAL_STRING(direct, viaPcap);
}

// --- A full five-frame set round-trips all of beacon/M1/M2/M3/M4. ---
void test_full_set_round_trips_all_five_frames(void) {
    CapturedHandshake original = makeHandshake("TargetNet", /*withM3M4=*/true);
    TEST_ASSERT_TRUE(original.isComplete());
    std::vector<uint8_t> bytes = serialize(original);

    CapturedHandshake parsed;
    TEST_ASSERT_TRUE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
    TEST_ASSERT_TRUE(parsed.isComplete());
    assertFrameEqual(original.beacon, parsed.beacon);
    for (int i = 0; i < 4; ++i) assertFrameEqual(original.msg[i], parsed.msg[i]);
}

// --- A hidden-SSID capture still round-trips the frames (buildHc22000Line rejects it downstream, not us). ---
void test_hidden_ssid_round_trips_but_empty_essid(void) {
    CapturedHandshake original = makeHandshake("");  // hidden network: empty SSID element.
    std::vector<uint8_t> bytes = serialize(original);

    CapturedHandshake parsed;
    TEST_ASSERT_TRUE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
    TEST_ASSERT_TRUE(parsed.isWpaSecValid());
    TEST_ASSERT_EQUAL_STRING("", parsed.ssid);
    char out[kHc22000LineMax];
    TEST_ASSERT_FALSE(buildHc22000Line(parsed, out, sizeof(out)));  // hidden => no WPA*01 line (ADR-0108).
}

// --- Fail loud: nullptr / too short for the global header. ---
void test_null_or_short_buffer_fails(void) {
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(nullptr, 100, parsed));
    std::vector<uint8_t> bytes = serialize(makeHandshake("TargetNet"));
    TEST_ASSERT_FALSE(parseHandshakePcap(bytes.data(), kPcapGlobalHeaderLen - 1, parsed));
    TEST_ASSERT_FALSE(parsed.isWpaSecValid());  // left empty.
}

// --- Fail loud: wrong magic / wrong link type. ---
void test_wrong_magic_fails(void) {
    std::vector<uint8_t> bytes = serialize(makeHandshake("TargetNet"));
    bytes[0] ^= 0xFF;  // corrupt the magic's first byte.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
}

void test_wrong_link_type_fails(void) {
    std::vector<uint8_t> bytes = serialize(makeHandshake("TargetNet"));
    bytes[20] = 0x01;  // network field (offset 20): not LINKTYPE_IEEE802_11 (105) anymore.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
}

// --- Fail loud: a frame body truncated by a short buffer. ---
void test_truncated_frame_body_fails(void) {
    std::vector<uint8_t> bytes = serialize(makeHandshake("TargetNet"));
    bytes.resize(bytes.size() - 5);  // chop the tail of the last record's frame.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
}

// --- Fail loud: a trailing partial record header (fewer than 16 bytes after the last frame). ---
void test_trailing_partial_record_header_fails(void) {
    std::vector<uint8_t> bytes = serialize(makeHandshake("TargetNet"));
    bytes.resize(bytes.size() + 8, 0x00);  // 8 stray bytes: start of a record header that never completes.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(bytes.data(), bytes.size(), parsed));
}

// Little-endian append helpers for hand-assembling corrupt/partial pcaps the serializer cannot produce.
static void putLe32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
static void putLe16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
static void putGlobalHeader(std::vector<uint8_t>& b) {
    putLe32(b, kPcapMagicMicroseconds);
    putLe16(b, kPcapVersionMajor);
    putLe16(b, kPcapVersionMinor);
    putLe32(b, 0);  // thisZone
    putLe32(b, 0);  // sigFigs
    putLe32(b, kPcapSnapLen);
    putLe32(b, kPcapLinkTypeIeee80211);
}
static void putRecord(std::vector<uint8_t>& b, const std::vector<uint8_t>& frame, uint32_t declaredInclLen) {
    putLe32(b, 0);                 // tsSec
    putLe32(b, 0);                 // tsUsec
    putLe32(b, declaredInclLen);   // inclLen (may be a lie, for the over-long test)
    putLe32(b, declaredInclLen);   // origLen
    b.insert(b.end(), frame.begin(), frame.end());
}

// --- Fail loud: a record header claiming a frame longer than kMaxFrameLen (corruption; cannot be stored). ---
// This lying-header variant (inclLen > actual body) is caught by the overrun check (cursor + inclLen > len).
void test_over_long_incl_len_fails(void) {
    std::vector<uint8_t> b;
    putGlobalHeader(b);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    putRecord(b, beacon, static_cast<uint32_t>(kMaxFrameLen + 1));  // header lies: frame too big to hold.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(b.data(), b.size(), parsed));
}

// --- Fail loud: the honest-but-too-big case the kMaxFrameLen bound ALONE guards. ---
// A record whose inclLen is in (kMaxFrameLen, 65535] AND whose body is fully present, so the overrun check
// (cursor + inclLen > len) passes. Without the kMaxFrameLen bound, absorbFrame would memcpy inclLen bytes
// into a kMaxFrameLen CapturedFrame and overrun it. This test bites that bound specifically (review MED #1).
void test_over_long_incl_len_with_full_body_fails(void) {
    std::vector<uint8_t> b;
    putGlobalHeader(b);
    const uint32_t tooBig = static_cast<uint32_t>(kMaxFrameLen) + 88;  // 600 bytes, all present below.
    std::vector<uint8_t> frame(tooBig, 0x00);
    frame[0] = 0x80;  // beacon FC, so absorbFrame would try to store it into out.beacon if the bound were gone.
    putRecord(b, frame, tooBig);  // inclLen == actual body length: the overrun check passes, only the bound rejects.
    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(b.data(), b.size(), parsed));
}

// --- Returns false on a structurally sound pcap that lacks M2 (not uploadable), with no partial handshake. ---
void test_incomplete_handshake_returns_false(void) {
    std::vector<uint8_t> b;
    putGlobalHeader(b);
    std::vector<uint8_t> beacon = buildBeacon(kBssid, "TargetNet");
    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    putRecord(b, beacon, static_cast<uint32_t>(beacon.size()));
    putRecord(b, m1, static_cast<uint32_t>(m1.size()));  // beacon + M1 only: not wpa-sec valid.

    CapturedHandshake parsed;
    TEST_ASSERT_FALSE(parseHandshakePcap(b.data(), b.size(), parsed));
    TEST_ASSERT_FALSE(parsed.hasBeacon());  // reset: no partial handshake leaks (ADR-0109 #2).
    TEST_ASSERT_FALSE(parsed.has(HandshakeMessage::M1));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_round_trip_reproduces_frames_bssid_ssid);
    RUN_TEST(test_hc22000_from_pcap_equals_hc22000_from_handshake);
    RUN_TEST(test_pmkid_round_trips_and_wpa02_from_pcap_equals_direct);
    RUN_TEST(test_full_set_round_trips_all_five_frames);
    RUN_TEST(test_hidden_ssid_round_trips_but_empty_essid);
    RUN_TEST(test_null_or_short_buffer_fails);
    RUN_TEST(test_wrong_magic_fails);
    RUN_TEST(test_wrong_link_type_fails);
    RUN_TEST(test_truncated_frame_body_fails);
    RUN_TEST(test_trailing_partial_record_header_fails);
    RUN_TEST(test_over_long_incl_len_fails);
    RUN_TEST(test_over_long_incl_len_with_full_body_fails);
    RUN_TEST(test_incomplete_handshake_returns_false);
    return UNITY_END();
}
