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
constexpr size_t kMaxWebhookUrlLen = 160;  ///< Optional push webhook URL (ADR-0023). A Discord webhook
                                           ///< URL is ~120 chars; 160 leaves margin. Empty = disabled.

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
    Maintenance,     ///< BOOT pressed during the boot splash: a hardened SoftAP serving the results
                     ///< dashboard instead of hunting (ADR-0039, entry per ADR-0053). Off the happy
                     ///< path — a hunt-suspended boot phase entered deliberately, left only by
                     ///< rebooting into Station.
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

/**
 * @brief Whether @p url is a usable push webhook endpoint (ADR-0023). Pure.
 *
 * The webhook is OPTIONAL and validated *separately* from the association triad, never folded into
 * validateCredentials(): a bad webhook URL must disable push, not send a good WiFi/wpa-sec triad to
 * the portal. Empty → unusable (push simply off). A non-empty URL must begin with "https://" — plain
 * http is refused so an alert is never sent unencrypted (fail loud, no silent downgrade) — and must
 * fit kMaxWebhookUrlLen. Deliberately not a full URL parse: the TLS handshake and the target host are
 * the real validators on device; this only gates the obvious unusable cases before wiring the surface.
 */
bool isUsableWebhookUrl(const char* url);

// Bounded STA-association attempts before a provisioned device falls back to the portal. A
// device that cannot reach its configured network re-opens for reconfiguration rather than
// looping silently (ADR-0006 decision #3; quality bar §3: fail loud, no masking fallback).
constexpr uint8_t kStaRetryBudget = 3;

/**
 * @brief Decide the entry phase at boot (ADR-0006 #3, extended by ADR-0039). Pure.
 *
 * Precedence:
 * - no usable stored triad → Provisioning (nothing to maintain or connect with — the button is
 *   ignored, so a first-boot BOOT press still lands on the setup portal);
 * - provisioned AND @p maintenanceRequested → Maintenance (the operator pressed BOOT during the boot
 *   splash to open the results dashboard over a hardened SoftAP, ADR-0039; entry gesture ADR-0053);
 * - provisioned, no button, STA-failure count still within kStaRetryBudget → StationConnect;
 * - otherwise → Provisioning (the STA-fail reconfiguration fallback, ADR-0006 #3).
 *
 * The button input repurposes the former re-provision signal (ADR-0039 decision 2): a BOOT press now
 * opens Maintenance rather than the portal. Re-provisioning a *working* device stays reachable via the
 * STA-fail fallback today, and will also be reachable from the config form Maintenance serves once
 * slice-0041 adds it; a results viewer, by contrast, had no entry at all before.
 */
Phase decideBootPhase(bool hasValidStoredCreds, uint8_t staFailCount, bool maintenanceRequested);

// Maintenance SoftAP passphrase bounds (ADR-0039 decision 6). Empty is allowed and means "fall back
// to kSoftApPassword"; any non-empty value must satisfy the same WPA2-PSK length window as an
// association passphrase, below which WiFi.softAP() would reject it — a loud validation failure at
// save time, never a silent downgrade to an open AP that would expose recovered PSKs to anyone.

/**
 * @brief Whether @p pass is a usable Maintenance SoftAP passphrase (ADR-0039 decision 6). Pure.
 *
 * Empty → usable (the AP falls back to the published kSoftApPassword; convenience over secrecy, the
 * operator's explicit choice). A non-empty value must be kMinPassLen..kMaxPassLen; a 1..7-character
 * value is rejected so the caller refuses the save rather than handing WiFi.softAP() a passphrase it
 * would silently drop. Bounded scan (one past the max), never trusting the caller to have terminated.
 */
bool isUsableMaintenancePass(const char* pass);

/**
 * @brief Format the SoftAP SSID as "Sapper-XXXX" from the last two bytes of @p mac (ADR-0006 #5).
 *
 * Upper-case hex, so a screenless board is provisionable by an operator who matched the name to
 * the README's pattern. The reference-to-array parameter fixes the buffer size at compile time.
 */
void formatSoftApSsid(const uint8_t mac[6], char (&out)[kSoftApSsidBufSize]);

/**
 * @brief The stable serial token for a phase, as it appears in the `[STATE] phase=<token>` line.
 *
 * This mapping is a contract, not a display string: the slice-0007 verify script greps for the
 * exact tokens (`provisioning`, `station_connect`, `time_sync`, `ready`) to assert a device
 * reached a phase (Scenarios C, D). A typo here silently breaks that grep, so the mapping is
 * pure and host-tested (ADR-0004 lane 1) rather than inlined into the device-only serial path.
 * Total over the enum: an added phase without a token fails the switch loudly, not silently.
 */
const char* phaseLabel(Phase phase);

}  // namespace sapper
