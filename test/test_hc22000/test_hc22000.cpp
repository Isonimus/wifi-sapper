/**
 * @file test_hc22000.cpp
 * @brief Native unit tests for pure WPA*01 hc22000 line synthesis (ADR-0108, §4 #45).
 *
 * Proves the hashline is built correctly from the whole captured frames: the exact field order and
 * lowercase-hex format against a known vector, that the ANonce is sourced from M1 (not M2) so the pair
 * is replay-counter-consistent (the correctness fix over the sibling Adversary), that the MIC is emitted
 * as its own field but zeroed inside the EAPOL blob, and the fail-closed paths (missing M1/M2, hidden
 * SSID, undersized buffer). No device needed (project CLAUDE.md §3).
 */
#include <unity.h>

#include <cstring>
#include <string>
#include <vector>

#include "net/hc22000.h"
#include "net/eapol.h"
#include "net/handshake_collector.h"

#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildEapol;

static const uint8_t kBssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kClient[6] = {0x06, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

// EAPOL field offsets from the EAPOL header start (mirrors hc22000.cpp / ADR-0108).
static constexpr uint16_t kNonceOffset = 17;
static constexpr uint16_t kNonceLen = 32;
static constexpr uint16_t kMicOffset = 81;
static constexpr uint16_t kMicLen = 16;

void setUp(void) {}
void tearDown(void) {}

static std::string toHex(const uint8_t* d, size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += kHex[d[i] >> 4];
        s += kHex[d[i] & 0x0F];
    }
    return s;
}

// Store a built frame into a CapturedFrame slot (whole frame, as the collector keeps it).
static void putFrame(CapturedFrame& slot, const std::vector<uint8_t>& frame) {
    TEST_ASSERT_TRUE(frame.size() <= kMaxFrameLen);
    std::memcpy(slot.data, frame.data(), frame.size());
    slot.len = static_cast<uint16_t>(frame.size());
}

// Poke `n` bytes into the EAPOL payload of `frame` at `eapolOffset`, in place.
static void pokeEapol(std::vector<uint8_t>& frame, uint16_t eapolOffset, const uint8_t* src, size_t n) {
    uint16_t eapolLen = 0;
    const uint8_t* eapol = locateEapol(frame.data(), static_cast<uint16_t>(frame.size()), eapolLen);
    TEST_ASSERT_NOT_NULL(eapol);
    TEST_ASSERT_TRUE(eapolOffset + n <= eapolLen);
    size_t base = static_cast<size_t>(eapol - frame.data()) + eapolOffset;
    std::memcpy(frame.data() + base, src, n);
}

// Build a handshake with a real M1 (carrying `anonce`) and M2 (carrying `snonce` + `mic`).
static CapturedHandshake makeHandshake(const char* ssid,
                                       const uint8_t anonce[32],
                                       const uint8_t snonce[32],
                                       const uint8_t mic[16],
                                       size_t m2ExtraKeyData = 0) {
    CapturedHandshake hs;
    std::memcpy(hs.bssid, kBssid, 6);
    std::strncpy(hs.ssid, ssid, sizeof(hs.ssid) - 1);

    std::vector<uint8_t> m1 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM1, true);
    pokeEapol(m1, kNonceOffset, anonce, kNonceLen);
    putFrame(hs.msg[0], m1);

    std::vector<uint8_t> m2 = buildEapol(kBssid, kClient, sapper_test::kKeyInfoM2, false, m2ExtraKeyData);
    pokeEapol(m2, kNonceOffset, snonce, kNonceLen);
    pokeEapol(m2, kMicOffset, mic, kMicLen);
    putFrame(hs.msg[1], m2);
    return hs;
}

// The exact WPA*01 line against a fully known capture.
void test_builds_wpa01_line_known_vector(void) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(0x01 + i);
    for (int i = 0; i < 32; ++i) snonce[i] = static_cast<uint8_t>(0x41 + i);
    for (int i = 0; i < 16; ++i) mic[i] = static_cast<uint8_t>(0xB0 + i);

    CapturedHandshake hs = makeHandshake("TestNet", anonce, snonce, mic);

    // Expected EAPOL blob: M2's EAPOL with the MIC field zeroed (hashcat recomputes it).
    uint16_t m2EapolLen = 0;
    const uint8_t* m2Eapol = locateEapol(hs.msg[1].data, hs.msg[1].len, m2EapolLen);
    TEST_ASSERT_NOT_NULL(m2Eapol);
    std::vector<uint8_t> eapolBlob(m2Eapol, m2Eapol + m2EapolLen);
    std::memset(eapolBlob.data() + kMicOffset, 0, kMicLen);

    std::string expected = "WPA*01*";
    expected += toHex(mic, 16);                                    expected += "*";
    expected += toHex(kBssid, 6);                                  expected += "*";
    expected += toHex(kClient, 6);                                 expected += "*";
    expected += toHex(reinterpret_cast<const uint8_t*>("TestNet"), 7); expected += "*";
    expected += toHex(anonce, 32);                                 expected += "*";
    expected += toHex(eapolBlob.data(), eapolBlob.size());         expected += "*00";

    char out[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(hs, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), out);
}

// The emitted ANonce is M1's, never M2's — the replay-counter-consistent pair (ADR-0108 decision #2).
void test_anonce_comes_from_m1_not_m2(void) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) anonce[i] = 0x11;   // M1 nonce: all 0x11.
    for (int i = 0; i < 32; ++i) snonce[i] = 0x22;   // M2 nonce: all 0x22 (must NOT appear as the ANonce).
    for (int i = 0; i < 16; ++i) mic[i] = 0x33;

    CapturedHandshake hs = makeHandshake("TestNet", anonce, snonce, mic);
    char out[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(hs, out, sizeof(out)));

    const std::string line(out);
    const std::string m1Nonce(64, '1');  // 32 bytes of 0x11 -> "11"*32.
    const std::string m2Nonce(64, '2');  // 32 bytes of 0x22.
    // The ANonce field is M1's; it appears once, delimited by '*' on both sides.
    TEST_ASSERT_TRUE(line.find("*" + m1Nonce + "*") != std::string::npos);
    // M2's SNonce rides inside the EAPOL blob but is never emitted as the standalone ANonce field.
    TEST_ASSERT_TRUE(line.find("*" + m2Nonce + "*") == std::string::npos);
}

// The MIC is its own field (real) but zeroed inside the EAPOL blob.
void test_mic_zeroed_in_blob_but_emitted_as_field(void) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) anonce[i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 32; ++i) snonce[i] = static_cast<uint8_t>(0x80 + i);
    for (int i = 0; i < 16; ++i) mic[i] = 0xCC;  // distinctive, non-zero.

    CapturedHandshake hs = makeHandshake("TestNet", anonce, snonce, mic);
    char out[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(hs, out, sizeof(out)));

    const std::string line(out);
    const std::string micHex(32, 'c');  // 16 bytes of 0xCC -> "cc"*16.
    // Present exactly once, as the field right after "WPA*01*".
    TEST_ASSERT_EQUAL_STRING("WPA*01*cccccccccccccccccccccccccccccccc*", line.substr(0, 40).c_str());
    // The EAPOL blob (last field before "*00") has the MIC region zeroed, so the 0xCC run appears once only.
    size_t first = line.find(micHex);
    TEST_ASSERT_TRUE(first != std::string::npos);
    TEST_ASSERT_TRUE(line.find(micHex, first + 1) == std::string::npos);
}

void test_missing_m1_returns_false(void) {
    uint8_t z32[32] = {0}, z16[16] = {0};
    CapturedHandshake hs = makeHandshake("TestNet", z32, z32, z16);
    hs.msg[0].len = 0;  // drop M1.
    char out[kHc22000LineMax];
    TEST_ASSERT_FALSE(buildHc22000Line(hs, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_missing_m2_returns_false(void) {
    uint8_t z32[32] = {0}, z16[16] = {0};
    CapturedHandshake hs = makeHandshake("TestNet", z32, z32, z16);
    hs.msg[1].len = 0;  // drop M2.
    char out[kHc22000LineMax];
    TEST_ASSERT_FALSE(buildHc22000Line(hs, out, sizeof(out)));
}

void test_hidden_ssid_returns_false(void) {
    uint8_t z32[32] = {0}, z16[16] = {0};
    CapturedHandshake hs = makeHandshake("", z32, z32, z16);  // hidden network: empty ESSID.
    char out[kHc22000LineMax];
    TEST_ASSERT_FALSE(buildHc22000Line(hs, out, sizeof(out)));
}

// A valid M2 with a large (but <= kMaxFrameLen) key-data section produces a line past 1024 chars; the
// output buffer (kHc22000LineMax) must cover it rather than silently fail (adversarial review finding #1).
void test_large_in_band_eapol_still_builds(void) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) { anonce[i] = static_cast<uint8_t>(i); snonce[i] = static_cast<uint8_t>(0x80 + i); }
    for (int i = 0; i < 16; ++i) mic[i] = static_cast<uint8_t>(0x40 + i);

    // 380 bytes of key-data: EAPOL ~479 bytes, frame ~511 (<= kMaxFrameLen), a capture the collector stores.
    CapturedHandshake hs = makeHandshake("TestNet", anonce, snonce, mic, 380);
    char out[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(hs, out, sizeof(out)));

    const std::string line(out);
    TEST_ASSERT_EQUAL_size_t(0, line.rfind("WPA*01*", 0));   // starts with the WPA*01 tag.
    TEST_ASSERT_TRUE(line.size() > 1024);                    // the valid line the old 1024 cap would have dropped.
    TEST_ASSERT_EQUAL_STRING("*00", line.substr(line.size() - 3).c_str());
}

// An implausible EAPOL declared-length field falls back to the located length and still builds (the trim
// fallback in hc22000.cpp), so a corrupt length word never under-builds a valid capture.
void test_implausible_declared_length_falls_back(void) {
    uint8_t anonce[32], snonce[32], mic[16];
    for (int i = 0; i < 32; ++i) { anonce[i] = static_cast<uint8_t>(0x10 + i); snonce[i] = static_cast<uint8_t>(0x50 + i); }
    for (int i = 0; i < 16; ++i) mic[i] = static_cast<uint8_t>(0x90 + i);

    CapturedHandshake hs = makeHandshake("TestNet", anonce, snonce, mic);
    uint16_t m2EapolLen = 0;
    const uint8_t* m2Eapol = locateEapol(hs.msg[1].data, hs.msg[1].len, m2EapolLen);
    TEST_ASSERT_NOT_NULL(m2Eapol);
    size_t base = static_cast<size_t>(m2Eapol - hs.msg[1].data);
    hs.msg[1].data[base + 2] = 0x00;
    hs.msg[1].data[base + 3] = 0x05;  // declared body = 5 -> total 9, under the 99-byte floor: must fall back.

    char out[kHc22000LineMax];
    TEST_ASSERT_TRUE(buildHc22000Line(hs, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(0, std::string(out).rfind("WPA*01*", 0));
}

void test_undersized_buffer_returns_false_no_partial(void) {
    uint8_t z32[32] = {0}, z16[16] = {0};
    CapturedHandshake hs = makeHandshake("TestNet", z32, z32, z16);
    char out[10];  // far too small for a full line.
    TEST_ASSERT_FALSE(buildHc22000Line(hs, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);  // no partial line left behind (quality bar §3).
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_builds_wpa01_line_known_vector);
    RUN_TEST(test_anonce_comes_from_m1_not_m2);
    RUN_TEST(test_mic_zeroed_in_blob_but_emitted_as_field);
    RUN_TEST(test_large_in_band_eapol_still_builds);
    RUN_TEST(test_implausible_declared_length_falls_back);
    RUN_TEST(test_missing_m1_returns_false);
    RUN_TEST(test_missing_m2_returns_false);
    RUN_TEST(test_hidden_ssid_returns_false);
    RUN_TEST(test_undersized_buffer_returns_false_no_partial);
    return UNITY_END();
}
