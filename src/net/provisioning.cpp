/**
 * @file provisioning.cpp
 * @brief Pure provisioning logic (ADR-0006). Host-tested; no Arduino, no hardware.
 */
#include "net/provisioning.h"

#include <cstdio>
#include <cstring>

namespace sapper {

CredentialError validateCredentials(const char* ssid, const char* pass, const char* key) {
    // strnlen with a bound one past the max: a field that hits the bound is over-length, so it
    // is rejected rather than measured further — never trusting the caller to have terminated.
    const size_t ssidLen = ssid ? strnlen(ssid, kMaxSsidLen + 1) : 0;
    const size_t passLen = pass ? strnlen(pass, kMaxPassLen + 1) : 0;
    const size_t keyLen = key ? strnlen(key, kMaxKeyLen + 1) : 0;

    if (ssidLen == 0) return CredentialError::SsidEmpty;
    if (ssidLen > kMaxSsidLen) return CredentialError::SsidTooLong;

    // Empty passphrase = an open network, allowed. A non-empty one must satisfy WPA2 bounds.
    if (passLen > 0 && passLen < kMinPassLen) return CredentialError::PassTooShort;
    if (passLen > kMaxPassLen) return CredentialError::PassTooLong;

    if (keyLen == 0) return CredentialError::KeyEmpty;

    return CredentialError::None;
}

bool isUsableWebhookUrl(const char* url) {
    if (url == nullptr) return false;
    // Bounded scan (one past the max): an over-length URL is unusable, never measured further or
    // trusted to be terminated — the same discipline validateCredentials() uses on the triad.
    const size_t len = strnlen(url, kMaxWebhookUrlLen + 1);
    if (len == 0 || len > kMaxWebhookUrlLen) return false;
    // Require TLS: a plaintext http:// webhook would send the alert unencrypted (§3, no silent
    // downgrade). strncmp, not a parse — the device TLS handshake is the real endpoint validator.
    return std::strncmp(url, "https://", 8) == 0;
}

Phase decideBootPhase(bool hasValidStoredCreds, uint8_t staFailCount, bool maintenanceRequested) {
    // Unprovisioned wins over the button: there is nothing to maintain and no network to join, so a
    // first-boot BOOT press still lands on the setup portal (ADR-0039 decision 2, entry ADR-0053).
    if (!hasValidStoredCreds) return Phase::Provisioning;
    // Provisioned + BOOT-held → the results dashboard over a hardened SoftAP (ADR-0039).
    if (maintenanceRequested) return Phase::Maintenance;
    // The normal hunt path, until the STA-retry budget is spent (ADR-0006 #3 reconfiguration fallback).
    if (staFailCount < kStaRetryBudget) return Phase::StationConnect;
    return Phase::Provisioning;
}

bool isUsableMaintenancePass(const char* pass) {
    if (pass == nullptr) return true;  // absent == empty == fall back to kSoftApPassword.
    // Bounded scan one past the max: an over-length value is unusable, never measured further or
    // trusted to be terminated — the discipline validateCredentials() uses on the triad.
    const size_t len = strnlen(pass, kMaxPassLen + 1);
    if (len == 0) return true;                 // empty → fall back to the published default AP password.
    return len >= kMinPassLen && len <= kMaxPassLen;  // else a real WPA2 passphrase, or a loud reject.
}

void formatSoftApSsid(const uint8_t mac[6], char (&out)[kSoftApSsidBufSize]) {
    // Last two MAC bytes, upper-case hex. snprintf bounds the write to the compile-time buffer.
    std::snprintf(out, kSoftApSsidBufSize, "Sapper-%02X%02X", mac[4], mac[5]);
}

const char* phaseLabel(Phase phase) {
    // No default case: -Wswitch then makes a newly-added phase without a token a compile error,
    // so the serial contract can never silently omit a state (quality bar §3: fail loud).
    switch (phase) {
        case Phase::Provisioning:   return "provisioning";
        case Phase::StationConnect: return "station_connect";
        case Phase::TimeSync:       return "time_sync";
        case Phase::Ready:          return "ready";
        case Phase::Maintenance:    return "maintenance";
    }
    // Reached only via a corrupted enum value (out-of-range cast / memory fault), never a real
    // Phase — surfaced as an obvious token rather than a plausible-looking one.
    return "invalid";
}

}  // namespace sapper
