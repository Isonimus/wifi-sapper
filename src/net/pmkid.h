/**
 * @file pmkid.h
 * @brief Pure PMKID extraction from an EAPOL-Key message-1 payload (ADR-0110).
 *
 * A clientless-PMKID capture is an M1 carrying a PMKID KDE in its Key Data; the PMKID is
 * `HMAC-SHA1(PMK, "PMK Name" | AP | STA)`, a crackable `WPA*02` artifact on its own. This reads it
 * out of the whole M1 the collector already keeps, at build time, like ADR-0108's `WPA*01`
 * extraction: ADR-0009 decision #3 keeps the collector parsing nothing on-device, and this read
 * happens in the pure, host-tested core (§4 #8), never in the driver callback.
 *
 * Carried from `../adversary/src/utils/pmkid_utils.h`, whose offsets were proven on-device
 * 2026-11-04 (adversary:ADR-0138). The two-byte Key-Data offset bug that kept the sibling's PMKID
 * counter at zero for the life of its feature is already fixed here: Key Data Length at 93, Key
 * Data at 95, measured from the key descriptor byte. No radio, no filesystem, no `Serial`;
 * host-unit-tested (ADR-0004 lane 1).
 */
#pragma once

#include <cstdint>

namespace sapper {

/// The PMKID length in bytes: the HMAC-SHA1 truncation a hashcat `WPA*02` line carries.
constexpr uint16_t kPmkidLen = 16;

/**
 * @brief Extract a PMKID from an EAPOL-Key message-1 payload.
 *
 * @param eapol    pointer to the EAPOL header (version/type/length/descriptor...), as
 *                 `locateEapol` (net/eapol.h) returns it.
 * @param eapolLen length of the EAPOL payload in bytes.
 * @param out      receives the 16-byte PMKID when one is found.
 * @return true when the Key Data carries a PMKID KDE (tag 0xDD, OUI 00-0F-AC, type 0x04); false for
 *         a null argument, a frame too short to hold the Key Data, a Key-Data-Length claiming more
 *         than is present, or a Key Data with no PMKID KDE.
 *
 * Bounds-safe: never reads past @p eapolLen, so a truncated or malformed frame returns false
 * rather than walking off the buffer (quality bar §3).
 */
bool extractPmkidFromEapol(const uint8_t* eapol, uint16_t eapolLen, uint8_t out[kPmkidLen]);

}  // namespace sapper
