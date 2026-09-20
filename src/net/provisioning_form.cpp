/**
 * @file provisioning_form.cpp
 * @brief Pure captive-portal form policy (ADR-0037). Host-tested; no Arduino, no hardware.
 */
#include "net/provisioning_form.h"

#include <cstdio>
#include <cstring>

namespace sapper {
namespace {

/// The HTML `checked` attribute when @p on, else nothing — so a rendered checkbox reflects stored state.
const char* checkedAttr(bool on) { return on ? " checked" : ""; }

}  // namespace

size_t buildSetupForm(const SetupFormModel& model, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;

    // When a secret is already stored, the field invites "leave blank to keep" and the key drops
    // `required` (so a re-save that keeps the key can submit an empty field). The stored *value* is
    // never rendered — only the fact that one exists (SetupFormModel carries no secret string, #20).
    const char* keyField =
        model.hasStoredKey
            ? "<input name='key' maxlength='64' placeholder='leave blank to keep current key'>"
            : "<input name='key' maxlength='64' required>";
    const char* webhookPlaceholder =
        model.hasStoredWebhook ? "leave blank to keep current webhook" : "https://ntfy.sh/your-topic";

    const int written = std::snprintf(
        out, outSize,
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Sapper setup</title></head><body>"
        "<h2>WiFi Sapper setup</h2>"
        "<form method='POST' action='/save'>"
        "<p>Network name (SSID)<br><input name='ssid' maxlength='32' required></p>"
        "<p>Passphrase (blank for open)<br><input name='pass' type='password' maxlength='63'></p>"
        "<p>wpa-sec API key<br>%s</p>"
        "<p>Push webhook URL (optional; ntfy or Discord, https)<br>"
        "<input name='webhook' type='url' maxlength='160' placeholder='%s'></p>"
        "<fieldset><legend>Push these (only if a webhook is set)</legend>"
        "<label><input name='nCracked' type='checkbox'%s> Cracked passwords</label><br>"
        "<label><input name='nCaptured' type='checkbox'%s> Handshake captures (one per network)</label><br>"
        "<label><input name='nSyncErr' type='checkbox'%s> Sync errors (appliance off-air)</label></fieldset>"
        "<p><label><input name='deauth' type='checkbox'%s> Enable deauth (arm)</label><br>"
        "<small>Only on networks you are authorized to test. Knocks clients off discovered APs to force "
        "handshakes. Off by default.</small></p>"
        "<p><button type='submit'>Save &amp; reboot</button></p>"
        "</form></body></html>",
        keyField, webhookPlaceholder, checkedAttr(model.notifyCracked), checkedAttr(model.notifyCaptured),
        checkedAttr(model.notifySyncError), checkedAttr(model.deauthArmed));

    // snprintf returns what it *would* have written: >= outSize means it truncated. Serve nothing
    // rather than a half-form (§3, fail loud) — clear out[0] so a careless caller cannot send garbage.
    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

void resolveProvisioningUpdate(const ProvisioningRecord& submitted, const ProvisioningRecord& stored,
                               bool hasStored, ProvisioningRecord& out) {
    // The pair (ssid/pass) and every toggle always come from the submission; start from it wholesale.
    out = submitted;
    if (!hasStored) return;  // first boot: nothing to keep — a blank key fails validation downstream.

    // A blank secret field keeps the stored value; a non-blank one already sits in `out` from the copy.
    if (submitted.key[0] == '\0') std::memcpy(out.key, stored.key, sizeof(out.key));
    if (submitted.webhookUrl[0] == '\0') std::memcpy(out.webhookUrl, stored.webhookUrl, sizeof(out.webhookUrl));
}

}  // namespace sapper
