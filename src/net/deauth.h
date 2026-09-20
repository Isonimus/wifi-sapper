/**
 * @file deauth.h
 * @brief Pure 802.11 deauth/disassoc management-frame construction (ADR-0027).
 *
 * The hardware-free "build one frame" half of the deauth actuation path: no `esp_wifi`, no `WiFi`,
 * no `Serial`. Given an AP BSSID and a destination MAC it lays out the fixed 26-byte management
 * frame a raw-TX seam then transmits. It is the write-side analogue of eapol.h's read-side parsing
 * (§4 invariant #8, #15): all frame *construction* lives here on the native lane, host-unit-tested
 * (ADR-0004 lane 1), so the byte layout is proved with no radio. The raw-TX capability that puts
 * these bytes on the air is a separate device seam (raw_transmitter.h, raw_transmitter_esp32.h); it
 * ships in every device build and is gated at runtime by the operator's default-off deauth arm toggle
 * (ADR-0029 superseded ADR-0027 decision 3's `SAPPER_TEST_HOOKS` compile-gate; §4 invariant #16).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// A deauthentication and a disassociation frame are byte-identical except for the frame-control
/// subtype (ADR-0027); the value is the first frame byte.
enum class ManagementSubtype : uint8_t {
    Deauth = 0xC0,    ///< Type management (00), subtype deauthentication (1100).
    Disassoc = 0xA0,  ///< Type management (00), subtype disassociation (1010).
};

/// 802.11 reason codes worth naming so the caller states intent, not a magic number. The full IEEE
/// table is large; only the codes this repo uses are surfaced (add on need, not speculatively).
enum class DeauthReason : uint16_t {
    Unspecified = 1,               ///< Generic; accepted by most clients.
    DeauthLeaving = 3,             ///< "Station is leaving" — the classic deauth reason.
    Class3FrameFromNonassoc = 7,   ///< Coaxes a client to reassociate (the reference's burst reason).
};

/// The fixed on-air length of a deauth or disassoc management frame: no variable body, no FCS
/// (the radio appends the FCS). Both subtypes are this length (ADR-0027).
constexpr size_t kDeauthFrameLen = 26;

/// The 6-byte broadcast address — deauthing it reaches every client associated to the AP at once,
/// which needs no station scanner (ADR-0027 decision 4). Callers may pass a specific client MAC
/// instead for a targeted deauth.
extern const uint8_t kBroadcastMac[6];

/**
 * @brief Build a deauth or disassoc management frame into @p out.
 *
 * Lays out the ADR-0027 byte order: FC subtype (@p subtype) + flags 0, duration 0, Addr1 = @p dest
 * (the client, or kBroadcastMac), Addr2 = Addr3 = @p bssid (the frame is spoofed *from* the AP),
 * sequence 0, and @p reason little-endian in the last two bytes.
 *
 * @return kDeauthFrameLen on success, or 0 if @p cap is smaller than kDeauthFrameLen — in which case
 *         nothing is written. A too-small buffer is refused, never truncated: a short management
 *         frame is a silently wrong actuation (quality-bar §3, fail loud).
 */
size_t buildDeauthFrame(uint8_t* out, size_t cap, ManagementSubtype subtype,
                        const uint8_t dest[6], const uint8_t bssid[6], DeauthReason reason);

}  // namespace sapper
