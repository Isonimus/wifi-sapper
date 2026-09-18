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

Phase decideBootPhase(bool hasValidStoredCreds, uint8_t staFailCount, bool reprovisionRequested) {
    if (hasValidStoredCreds && !reprovisionRequested && staFailCount < kStaRetryBudget) {
        return Phase::StationConnect;
    }
    return Phase::Provisioning;
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
    }
    // Reached only via a corrupted enum value (out-of-range cast / memory fault), never a real
    // Phase — surfaced as an obvious token rather than a plausible-looking one.
    return "invalid";
}

}  // namespace sapper
