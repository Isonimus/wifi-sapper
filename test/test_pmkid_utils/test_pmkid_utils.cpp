/**
 * @file test_pmkid_utils.cpp
 * @brief Native unit tests for pure PMKID extraction from an EAPOL-Key M1 (ADR-0110, §4 #47).
 *
 * The one thing that must stay true is the Key-Data offset: the sibling's PMKID counter sat at
 * zero for the life of its feature because Key Data was read two bytes early (into the Key MIC
 * field), so the KDE walk always desynced. The wire layout below is hardcoded ON PURPOSE and NOT
 * via the function-under-test's own constants: building and parsing with the same constant would
 * be tautological and the original off-by-two would pass. From the Key Descriptor byte (EAPOL
 * header already skipped): KeyInfo(2)+KeyLen(2)+Replay(8)+Nonce(32)+IV(16)+RSC(8)+KeyID(8)+MIC(16)
 * puts the Key Data Length at keyFrame[93..94] and the Key Data at keyFrame[95]. No device needed.
 */
#include <unity.h>

#include <cstring>

#include "net/pmkid.h"

using namespace sapper;

// Ground-truth 802.11 EAPOL-Key offsets, independent of net/pmkid's own constants (see the header
// comment): the fixture encodes the real wire layout so a wrong offset fails here.
static constexpr uint16_t kWireEapolHeader = 4;
static constexpr uint16_t kWireKeyDataLenOff = 93;  // within the key frame.
static constexpr uint16_t kWireKeyDataOff = 95;     // within the key frame.

// PMKID KDE bytes, hardcoded (not via net/pmkid constants): DD 14 00-0F-AC 04 <16>.
static constexpr uint8_t kKdeTag = 0xDD;
static constexpr uint8_t kKdeLen = 0x14;  // 20 = OUI(3)+Type(1)+PMKID(16)

void setUp(void) {}
void tearDown(void) {}

// Build a byte-accurate EAPOL-Key M1 payload (from the EAPOL header) with the given Key Data
// appended. The fixed portion is 4 (EAPOL header) + 95 = 99 bytes.
static uint16_t buildM1(uint8_t* buf, const uint8_t* keyData, uint16_t keyDataLen) {
    const uint16_t fixed = kWireEapolHeader + kWireKeyDataOff;  // 99
    std::memset(buf, 0, fixed);
    buf[0] = 0x02;  // EAPOL version.
    buf[1] = 0x03;  // EAPOL type: Key.
    const uint16_t remaining = kWireKeyDataOff + keyDataLen;  // body after the 4-byte header.
    buf[2] = (remaining >> 8) & 0xFF;
    buf[3] = remaining & 0xFF;
    buf[4] = 0x02;  // Key descriptor type: RSN.
    uint8_t* keyFrame = buf + kWireEapolHeader;
    keyFrame[kWireKeyDataLenOff] = (keyDataLen >> 8) & 0xFF;
    keyFrame[kWireKeyDataLenOff + 1] = keyDataLen & 0xFF;
    std::memcpy(keyFrame + kWireKeyDataOff, keyData, keyDataLen);
    return fixed + keyDataLen;
}

// A single PMKID KDE: DD 14 00-0F-AC 04 <16 bytes>.
static uint16_t makePmkidKde(uint8_t* kde, const uint8_t pmkid[16]) {
    kde[0] = kKdeTag;
    kde[1] = kKdeLen;
    kde[2] = 0x00; kde[3] = 0x0F; kde[4] = 0xAC;  // OUI.
    kde[5] = 0x04;                                 // type 4 = PMKID.
    std::memcpy(kde + 6, pmkid, 16);
    return 22;
}

static const uint8_t kPmkid[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

void test_extracts_pmkid_from_m1(void) {
    uint8_t kde[22];
    const uint16_t kdeLen = makePmkidKde(kde, kPmkid);
    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, kde, kdeLen);

    uint8_t out[kPmkidLen];
    TEST_ASSERT_TRUE(extractPmkidFromEapol(frame, frameLen, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kPmkid, out, kPmkidLen);
}

void test_finds_pmkid_after_another_element(void) {
    // A benign element (tag 0x30, len 4) precedes the PMKID KDE: the walk must skip it and still land.
    uint8_t keyData[40];
    keyData[0] = 0x30;
    keyData[1] = 0x04;
    std::memset(keyData + 2, 0xAB, 4);
    const uint16_t kdeLen = makePmkidKde(keyData + 6, kPmkid);
    const uint16_t keyDataLen = 6 + kdeLen;

    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, keyData, keyDataLen);

    uint8_t out[kPmkidLen];
    TEST_ASSERT_TRUE(extractPmkidFromEapol(frame, frameLen, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kPmkid, out, kPmkidLen);
}

void test_finds_pmkid_after_zero_length_element(void) {
    // A zero-length element (tag, len=0) precedes the PMKID KDE: the walk must skip it by the full
    // 2-byte tag+len header and still land on the PMKID, not desync by advancing one byte. Regression
    // for the KDE-walk zero-length advance (adversarial review, slice-0111).
    uint8_t keyData[40];
    keyData[0] = 0xAA;  // some element tag.
    keyData[1] = 0x00;  // length 0 -> a 2-byte element.
    const uint16_t kdeLen = makePmkidKde(keyData + 2, kPmkid);
    const uint16_t keyDataLen = 2 + kdeLen;

    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, keyData, keyDataLen);

    uint8_t out[kPmkidLen];
    TEST_ASSERT_TRUE(extractPmkidFromEapol(frame, frameLen, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kPmkid, out, kPmkidLen);
}

void test_no_pmkid_when_kde_absent(void) {
    // Key Data holds only a non-PMKID vendor KDE (type 0x01, e.g. a GTK): no PMKID.
    uint8_t keyData[24];
    keyData[0] = kKdeTag;
    keyData[1] = kKdeLen;
    keyData[2] = 0x00; keyData[3] = 0x0F; keyData[4] = 0xAC;
    keyData[5] = 0x01;  // not PMKID (type 4).
    std::memset(keyData + 6, 0x77, 16);

    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, keyData, 22);

    uint8_t out[kPmkidLen];
    TEST_ASSERT_FALSE(extractPmkidFromEapol(frame, frameLen, out));
}

void test_no_pmkid_when_key_data_empty(void) {
    // M1 with no Key Data (Key Data Length = 0): the common 4-way M1.
    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, nullptr, 0);

    uint8_t out[kPmkidLen];
    TEST_ASSERT_FALSE(extractPmkidFromEapol(frame, frameLen, out));
}

void test_rejects_truncated_frame(void) {
    // Shorter than the fixed EAPOL-Key header: must bail, not read out of bounds.
    uint8_t frame[64];
    std::memset(frame, 0, sizeof(frame));
    uint8_t out[kPmkidLen];
    TEST_ASSERT_FALSE(extractPmkidFromEapol(frame, 50, out));
}

void test_rejects_overlong_key_data_len(void) {
    // Key Data Length claims more bytes than the frame carries: must not walk past the buffer.
    uint8_t kde[22];
    makePmkidKde(kde, kPmkid);
    uint8_t frame[256];
    const uint16_t frameLen = buildM1(frame, kde, 22);
    uint8_t* keyFrame = frame + kWireEapolHeader;
    keyFrame[kWireKeyDataLenOff] = 0x00;
    keyFrame[kWireKeyDataLenOff + 1] = 0xC8;  // claim 200 bytes.

    uint8_t out[kPmkidLen];
    TEST_ASSERT_FALSE(extractPmkidFromEapol(frame, frameLen, out));
}

void test_null_arguments_return_false(void) {
    uint8_t out[kPmkidLen];
    uint8_t frame[128] = {0};
    TEST_ASSERT_FALSE(extractPmkidFromEapol(nullptr, 128, out));
    TEST_ASSERT_FALSE(extractPmkidFromEapol(frame, 128, nullptr));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_extracts_pmkid_from_m1);
    RUN_TEST(test_finds_pmkid_after_another_element);
    RUN_TEST(test_finds_pmkid_after_zero_length_element);
    RUN_TEST(test_no_pmkid_when_kde_absent);
    RUN_TEST(test_no_pmkid_when_key_data_empty);
    RUN_TEST(test_rejects_truncated_frame);
    RUN_TEST(test_rejects_overlong_key_data_len);
    RUN_TEST(test_null_arguments_return_false);
    return UNITY_END();
}
