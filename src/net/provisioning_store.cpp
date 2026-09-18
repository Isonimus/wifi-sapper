/**
 * @file provisioning_store.cpp
 * @brief NVS-backed provisioning store over Arduino `Preferences` (ADR-0006). Device-only.
 */
#include "net/provisioning_store.h"

#include <Preferences.h>

#include <cstring>

namespace sapper {
namespace {

// The one NVS namespace and key set for provisioned secrets. File-local so the single-writer
// seam (CLAUDE.md §4 invariant #6) is a grep away: these names appear in this file and nowhere
// else. NVS caps namespace and key names at 15 chars — all four are within it.
constexpr char kNamespace[] = "sapper";
constexpr char kKeySsid[] = "wifi_ssid";
constexpr char kKeyPass[] = "wifi_pass";
constexpr char kKeyKey[] = "wpasec_key";

}  // namespace

bool loadProvisioning(ProvisioningRecord& out) {
    // Zero first: on any early return the caller reads a clean record, never a stale stack buffer.
    std::memset(&out, 0, sizeof(out));

    Preferences prefs;
    // Read-only begin returns false when the namespace was never created — i.e. first boot,
    // which is "not provisioned", not an error.
    if (!prefs.begin(kNamespace, /*readOnly=*/true)) {
        return false;
    }
    prefs.getString(kKeySsid, out.ssid, sizeof(out.ssid));
    prefs.getString(kKeyPass, out.pass, sizeof(out.pass));
    prefs.getString(kKeyKey, out.key, sizeof(out.key));
    prefs.end();

    // A half-written or corrupt triad reads as "not provisioned" so the boot gate opens the
    // portal rather than trying to associate with junk (fail loud, no masking fallback).
    return validateCredentials(out.ssid, out.pass, out.key) == CredentialError::None;
}

bool persistProvisioning(const ProvisioningRecord& record) {
    // Validate before opening NVS: an invalid triad must never overwrite a good stored record.
    if (validateCredentials(record.ssid, record.pass, record.key) != CredentialError::None) {
        return false;
    }

    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
        return false;  // NVS unavailable — fail loud; do not report a store that did not happen.
    }

    // SSID is written last: loadProvisioning() treats an empty SSID as "not provisioned", so if
    // a write is cut short (e.g. NVS exhaustion) the record fails validation and the next boot
    // falls back to the portal — never associates with a half-updated credential set. Each
    // putString returns the bytes stored; a short count is a failed write (0 is valid only for
    // the open-network empty passphrase, where strlen is also 0).
    const bool wrote = prefs.putString(kKeyPass, record.pass) == std::strlen(record.pass) &&
                       prefs.putString(kKeyKey, record.key) == std::strlen(record.key) &&
                       prefs.putString(kKeySsid, record.ssid) == std::strlen(record.ssid);
    prefs.end();
    return wrote;
}

void clearProvisioning() {
    Preferences prefs;
    if (!prefs.begin(kNamespace, /*readOnly=*/false)) {
        return;  // namespace absent or NVS down — nothing to clear.
    }
    prefs.clear();  // wipe every key in the namespace, forcing the next boot into the portal.
    prefs.end();
}

}  // namespace sapper
