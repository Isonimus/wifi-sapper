/**
 * @file provisioning.h
 * @brief Pure provisioning logic: credential validation, the boot gate, SoftAP naming (ADR-0006).
 *
 * This is the hardware-free half of slice-0007 — no `Preferences`, `WiFi`, or `Serial`. It is
 * host-unit-tested (ADR-0004 lane 1) so the two decisions that gate every later network action
 * — "are these credentials usable?" and "portal or straight to STA?" — are proven off-device.
 * The NVS store, captive portal, and STA/NTP that surround it are device-only and proven by the
 * slice-0007 verify script (Scenarios C, D).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

// Credential field bounds. Named, not bare, because each is a protocol limit a reader must be
// able to trace: SSID and passphrase are the 802.11 / WPA2 maxima, so a value past them could
// never associate and is a validation failure, not a truncation to paper over (quality bar §3).
constexpr size_t kMaxSsidLen = 32;   ///< 802.11 SSID: 32 octets.
constexpr size_t kMinPassLen = 8;    ///< WPA2 PSK passphrase minimum.
constexpr size_t kMaxPassLen = 63;   ///< WPA2 PSK passphrase maximum (64 = a raw 256-bit PSK).
constexpr size_t kMaxKeyLen = 64;    ///< wpa-sec API key; generous upper bound.

/// Buffer size for a SoftAP SSID "Sapper-XXXX": 7 + 4 hex + NUL. A compile-time size so
/// formatSoftApSsid() cannot be handed a too-small buffer (no runtime cap, no silent truncation).
constexpr size_t kSoftApSsidBufSize = 12;

/**
 * @brief The pre-engine boot phases, reported on the serial `[STATE]` line (ADR-0006 decision #3).
 *
 * Ordered as the provisioned happy path traverses them. Each is a state that blocks before the
 * engine loop, so each must pump the serial channel (CLAUDE.md §4 invariant #3). `Ready` is the
 * headless steady state this slice reaches; the capture engine slots in there at slice-3/4.
 */
enum class Phase : uint8_t {
    Provisioning,    ///< Captive portal up, awaiting credentials.
    StationConnect,  ///< Associating to the configured network.
    TimeSync,        ///< NTP sync in progress; the TLS validity window depends on it.
    Ready,           ///< Provisioned, associated, clock synced. Engine mounts here later.
};

/**
 * @brief Why a credential triad was rejected. `None` means the triad is usable.
 *
 * A specific reason rather than a bare bool: the portal echoes it back to the operator, and a
 * validator that only says "invalid" cannot tell a too-short passphrase from a missing key.
 */
enum class CredentialError : uint8_t {
    None,
    SsidEmpty,
    SsidTooLong,
    PassTooShort,  ///< Non-empty but under kMinPassLen — an open network uses an empty passphrase.
    PassTooLong,
    KeyEmpty,
};

/**
 * @brief Validate a credential triad against the 802.11/WPA2 bounds. Pure; no truncation.
 *
 * An empty passphrase is accepted (an open network); a non-empty one must be kMinPassLen..
 * kMaxPassLen. SSID must be 1..kMaxSsidLen. The wpa-sec key must be non-empty — the appliance
 * cannot upload without it, so an empty key is a hard failure here, not a deferred surprise at
 * first upload. Checks length by scanning, so an over-long field fails rather than being cut to
 * fit a buffer.
 */
CredentialError validateCredentials(const char* ssid, const char* pass, const char* key);

// Bounded STA-association attempts before a provisioned device falls back to the portal. A
// device that cannot reach its configured network re-opens for reconfiguration rather than
// looping silently (ADR-0006 decision #3; quality bar §3: fail loud, no masking fallback).
constexpr uint8_t kStaRetryBudget = 3;

/**
 * @brief Decide the entry phase at boot (ADR-0006 decision #3). Pure.
 *
 * Straight to StationConnect only when stored credentials are valid, re-provisioning was not
 * requested, and the STA-failure count is still within kStaRetryBudget. Every other case —
 * absent/invalid credentials, a re-provision request (GPIO/button hold), or the retry budget
 * spent — opens the portal.
 */
Phase decideBootPhase(bool hasValidStoredCreds, uint8_t staFailCount, bool reprovisionRequested);

/**
 * @brief Format the SoftAP SSID as "Sapper-XXXX" from the last two bytes of @p mac (ADR-0006 #5).
 *
 * Upper-case hex, so a screenless board is provisionable by an operator who matched the name to
 * the README's pattern. The reference-to-array parameter fixes the buffer size at compile time.
 */
void formatSoftApSsid(const uint8_t mac[6], char (&out)[kSoftApSsidBufSize]);

}  // namespace sapper
