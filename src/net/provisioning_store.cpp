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
constexpr char kKeyWebhook[] = "webhook_url";  // 11 chars, within NVS's 15-char key limit.

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
    // Optional (ADR-0023): absent on a device provisioned before webhooks existed, which getString
    // leaves as the zeroed default — an empty URL means "push disabled", not a load failure, so it
    // never affects the validity gate below.
    prefs.getString(kKeyWebhook, out.webhookUrl, sizeof(out.webhookUrl));
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

    // Each putString returns the bytes stored; a short count is a failed write (0 is valid only
    // for the open-network empty passphrase, where strlen is also 0). All three are attempted
    // unconditionally rather than short-circuited: a short-circuit would skip the SSID write after
    // an earlier failure and, on a *re-provision*, leave the previous SSID and key sitting beside
    // the newly written pass — a mixed triad that still passes validateCredentials() and loads as
    // usable, associating with wrong credentials. On any failed write we clear the namespace, so
    // the store holds a complete triad or nothing — never a mix (provisioning_store.h) — and the
    // next boot falls back to the portal instead of a half-updated credential set.
    const bool wrotePass = prefs.putString(kKeyPass, record.pass) == std::strlen(record.pass);
    const bool wroteKey = prefs.putString(kKeyKey, record.key) == std::strlen(record.key);
    const bool wroteSsid = prefs.putString(kKeySsid, record.ssid) == std::strlen(record.ssid);
    // The webhook URL is optional (ADR-0023): an empty value stores 0 bytes, which equals its strlen,
    // so it counts as written and an unconfigured push does not fail the persist. It rides the same
    // all-or-nothing clear() below, so a partial write never leaves a webhook beside a stale triad.
    const bool wroteWebhook =
        prefs.putString(kKeyWebhook, record.webhookUrl) == std::strlen(record.webhookUrl);
    const bool wrote = wrotePass && wroteKey && wroteSsid && wroteWebhook;
    if (!wrote) {
        prefs.clear();  // partial write — wipe so no mixed triad can survive to the next boot.
    }
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

void seedProvisioning(const ProvisioningRecord& seed) {
    // Persist a valid seed; on an invalid one (e.g. a hooks build with no credential flags) clear
    // any stored triad so the store reflects exactly this seed rather than a prior flash's creds.
    if (!persistProvisioning(seed)) {
        clearProvisioning();
    }
}

}  // namespace sapper
