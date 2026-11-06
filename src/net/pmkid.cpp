/**
 * @file pmkid.cpp
 * @brief Pure PMKID extraction (ADR-0110). See pmkid.h.
 */
#include "net/pmkid.h"

#include <cstring>

namespace sapper {
namespace {

// EAPOL-Key field offsets from the Key Descriptor byte (the EAPOL 4-byte header already skipped),
// from the 802.11 EAPOL-Key layout: KeyInfo(2) + KeyLen(2) + Replay(8) + Nonce(32) + IV(16) +
// RSC(8) + KeyID(8) + MIC(16) places the Key Data Length field at 93 and the Key Data at 95.
// Mirrors hc22000.cpp's header-relative offsets (its Key Data Length sits at 97 = 93 + the 4-byte
// EAPOL header). Proven on-device 2026-11-04 (adversary:ADR-0138).
constexpr uint16_t kEapolHeaderLen = 4;
constexpr uint16_t kKeyDataLenOffset = 93;  // within the key frame (post-header)
constexpr uint16_t kKeyDataOffset = 95;     // within the key frame (post-header)

// PMKID KDE inside Key Data: Tag=0xDD, Len, OUI=00-0F-AC, Type=0x04, PMKID(16).
constexpr uint8_t kKdeTag = 0xDD;
constexpr uint8_t kKdeOui0 = 0x00, kKdeOui1 = 0x0F, kKdeOui2 = 0xAC;
constexpr uint8_t kKdeType = 0x04;
constexpr uint8_t kKdeMinLen = 20;      // OUI(3) + Type(1) + PMKID(16)
constexpr uint8_t kKdeValueOffset = 6;  // Tag(1) + Len(1) + OUI(3) + Type(1)

}  // namespace

bool extractPmkidFromEapol(const uint8_t* eapol, uint16_t eapolLen, uint8_t out[kPmkidLen]) {
    if (eapol == nullptr || out == nullptr) return false;
    // Need the EAPOL header plus everything up to the Key Data.
    if (eapolLen < kEapolHeaderLen + kKeyDataOffset) return false;

    const uint8_t* keyFrame = eapol + kEapolHeaderLen;
    const uint16_t keyFrameLen = eapolLen - kEapolHeaderLen;

    const uint16_t keyDataLen =
        (static_cast<uint16_t>(keyFrame[kKeyDataLenOffset]) << 8) | keyFrame[kKeyDataLenOffset + 1];
    if (keyDataLen == 0) return false;
    if (kKeyDataOffset + keyDataLen > keyFrameLen) return false;  // claims more than present.

    const uint8_t* keyData = keyFrame + kKeyDataOffset;

    // Walk the Key Data element list (Tag, Len, Value) looking for the PMKID KDE.
    uint16_t i = 0;
    while (i + 2 <= keyDataLen) {
        const uint8_t tag = keyData[i];
        const uint8_t len = keyData[i + 1];
        if (i + 2 + len > keyDataLen) break;  // element claims past the buffer.
        if (tag == kKdeTag && len >= kKdeMinLen &&
            keyData[i + 2] == kKdeOui0 && keyData[i + 3] == kKdeOui1 &&
            keyData[i + 4] == kKdeOui2 && keyData[i + 5] == kKdeType) {
            std::memcpy(out, keyData + i + kKdeValueOffset, kPmkidLen);
            return true;
        }
        // Advance past this element (tag + len header + its value). A zero-length element advances
        // by exactly 2, so the walk never stalls and never desyncs: a prior `len==0 -> ++i` special
        // case advanced by one byte and mis-parsed the next element (adversarial review, slice-0111).
        i += 2 + len;
    }
    return false;
}

}  // namespace sapper
