/**
 * @file provisioning_form.cpp
 * @brief Pure captive-portal form policy (ADR-0037). Host-tested; no Arduino, no hardware.
 */
#include "net/provisioning_form.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "net/portal_theme.h"  // kPortalThemeCss, kPortalWordmarkSvg — the shared banner-matching look.

namespace sapper {
namespace {

/// The HTML `checked` attribute when @p on, else nothing — so a rendered checkbox reflects stored state.
const char* checkedAttr(bool on) { return on ? " checked" : ""; }

}  // namespace

size_t buildSetupForm(const SetupFormModel& model, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) return 0;

    // When a network is already stored, the SSID drops `required` and both Wi-Fi fields invite "leave
    // blank to keep current Wi-Fi" (ADR-0106): a blank SSID keeps the whole stored pair, so the operator
    // can edit anything else without re-typing the network. The SSID value is never rendered (echoing it
    // on the public setup AP would leak the home network name) — only the fact one is stored, like the
    // secrets. First boot keeps the SSID `required` and teaches the WPA2 length / Open checkbox instead.
    const char* ssidField =
        model.hasStoredNetwork
            ? "<input name='ssid' maxlength='32' autocapitalize='none' autocorrect='off' "
              "spellcheck='false' placeholder='leave blank to keep current Wi-Fi'>"
            : "<input name='ssid' maxlength='32' autocapitalize='none' autocorrect='off' "
              "spellcheck='false' required>";
    const char* passField =
        model.hasStoredNetwork
            ? "<input name='pass' type='password' maxlength='63' "
              "placeholder='leave blank to keep current Wi-Fi password'>"
            : "<input name='pass' type='password' maxlength='63' placeholder='at least 8 characters'>";
    const char* wifiHelp =
        model.hasStoredNetwork
            ? "<small>Leave the network name blank to keep your current Wi-Fi. To change only the "
              "passphrase, leave the name and type the new one. To switch networks, type the new name and "
              "its passphrase. Tick Open for a passphrase-less network.</small>"
            : "<small>Tick Open only for a network with no passphrase.</small>";
    // When a secret is already stored, the field invites "leave blank to keep" and the key drops
    // `required` (so a re-save that keeps the key can submit an empty field). The stored *value* is
    // never rendered — only the fact that one exists (SetupFormModel carries no secret string, #20).
    const char* keyField =
        model.hasStoredKey
            ? "<input name='key' maxlength='64' placeholder='leave blank to keep current key'>"
            : "<input name='key' maxlength='64' required>";
    const char* webhookPlaceholder =
        model.hasStoredWebhook ? "leave blank to keep current webhook" : "https://ntfy.sh/your-topic";
    // The Maintenance AP passphrase (ADR-0039 #6): optional (empty → the default AP password), so never
    // `required`. Like the key/webhook, the stored value is never rendered — only "leave blank to keep".
    const char* maintPlaceholder = model.hasStoredMaintenancePass
                                       ? "leave blank to keep current passphrase"
                                       : "blank = use default AP password";

    const int written = std::snprintf(
        out, outSize,
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>WiFi Sapper setup</title>%s</head><body><div class='wrap'>%s"
        "<h2>Device setup</h2>"
        "<form method='POST' action='/save'>"
        // A mobile keyboard autocapitalising/autocorrecting the SSID stores a wrong name (SSIDs are
        // case-sensitive, arbitrary text), which fails association as a silent credential error. The
        // password-type fields below are already shielded by type=password; this plain text field is not.
        "<p>Network name (SSID)<br>%s</p>"
        "<p>Passphrase<br>%s<br>"
        "<label><input name='open' type='checkbox'> Open network (no passphrase)</label><br>%s</p>"
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
        "<p>Maintenance dashboard passphrase (min 8 chars)<br>"
        "<input name='maintpass' type='password' maxlength='63' placeholder='%s'><br>"
        "<small>Protects the results dashboard shown when you press BOOT during the boot splash. Blank "
        "uses the default AP password.</small></p>"
        // Inactivity screen dimming (ADR-0093): two idle delays in seconds. Prefilled with the stored
        // value (not a secret), so a re-save to change something else preserves them, like the checkboxes
        // preserve their state. 0 dim = off; a nonzero off is clamped >= dim on save.
        "<p>Screen dim after (seconds idle; 0 = never dim)<br>"
        "<input name='dimAfter' type='number' min='0' max='65535' inputmode='numeric' value='%u'></p>"
        "<p>Screen off after (seconds idle; 0 = dim only)<br>"
        "<input name='offAfter' type='number' min='0' max='65535' inputmode='numeric' value='%u'></p>"
        "<p><button type='submit'>Save &amp; reboot</button></p>"
        "</form></div></body></html>",
        kPortalThemeCss, kPortalWordmarkSvg, ssidField, passField, wifiHelp,
        keyField, webhookPlaceholder, checkedAttr(model.notifyCracked), checkedAttr(model.notifyCaptured),
        checkedAttr(model.notifySyncError), checkedAttr(model.deauthArmed), maintPlaceholder,
        static_cast<unsigned>(model.screenDimAfterSec), static_cast<unsigned>(model.screenOffAfterSec));

    // snprintf returns what it *would* have written: >= outSize means it truncated. Serve nothing
    // rather than a half-form (§3, fail loud) — clear out[0] so a careless caller cannot send garbage.
    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

void resolveProvisioningUpdate(const ProvisioningRecord& submitted, const ProvisioningRecord& stored,
                               bool hasStored, bool openNetwork, ProvisioningRecord& out) {
    // Every toggle, the dim delays, and the submitted strings start from the submission wholesale.
    out = submitted;
    // The screen-dim delays (ADR-0093) came from the submission via the copy; clamp the off delay, when
    // set, to >= the dim delay so the readable stage is never skipped. Applied before the first-boot
    // return so a fresh config is sane too; the pure predicate is safe even if this is bypassed. A 0 off
    // (dim-only) is left as-is.
    if (out.screenOffAfterSec != 0 && out.screenOffAfterSec < out.screenDimAfterSec) {
        out.screenOffAfterSec = out.screenDimAfterSec;
    }

    // The Wi-Fi SSID is keep-on-blank (ADR-0106): on a provisioned device a blank submitted SSID keeps the
    // stored network *name*, so the operator can edit anything else without re-typing it. The kept name is
    // unambiguous, so the passphrase is then set independently against it: Open ticked makes the current
    // network open, a blank passphrase keeps the stored one (the full light-touch edit), and a *typed*
    // passphrase rotates it (change the password without re-typing the SSID) — the old passphrase is never
    // kept over a typed one, which would reboot with a stale password and strand the unit. When the SSID is
    // changed instead (non-blank submission, or first boot), the passphrase comes wholly from the
    // submission, so a stored passphrase is never bound to a different SSID than it was stored with; Open
    // then forces it empty, and a blank-pass new network with Open *unticked* is refused by
    // validateNetworkEntry at the save site (never silently stored open here — the brick footgun).
    if (hasStored && submitted.ssid[0] == '\0') {
        std::memcpy(out.ssid, stored.ssid, sizeof(out.ssid));  // keep the network name
        if (openNetwork) {
            out.pass[0] = '\0';  // switch the current network to open
        } else if (submitted.pass[0] == '\0') {
            std::memcpy(out.pass, stored.pass, sizeof(out.pass));  // keep the current passphrase
        }
        // else: a new passphrase was typed for the same network — keep it (already copied into `out`).
    } else if (openNetwork) {
        out.pass[0] = '\0';  // a new network explicitly marked open
    }

    if (!hasStored) return;  // first boot: no secret to keep — a blank key fails validation downstream.

    // A blank secret field keeps the stored value; a non-blank one already sits in `out` from the copy.
    if (submitted.key[0] == '\0') std::memcpy(out.key, stored.key, sizeof(out.key));
    if (submitted.webhookUrl[0] == '\0') std::memcpy(out.webhookUrl, stored.webhookUrl, sizeof(out.webhookUrl));
    // The Maintenance AP passphrase (ADR-0039 #6) is a secret handled like the key/webhook: a blank
    // re-save keeps the stored passphrase rather than dropping the AP back to the default password.
    if (submitted.maintenancePass[0] == '\0')
        std::memcpy(out.maintenancePass, stored.maintenancePass, sizeof(out.maintenancePass));
}

NetworkEntryError validateNetworkEntry(const char* submittedSsid, const char* submittedPass,
                                       bool openNetwork, bool hasStored) {
    // Keeping the current network (a stored record, SSID left blank): the passphrase/Open fields are
    // irrelevant — resolveProvisioningUpdate keeps the stored pair.
    if (hasStored && (submittedSsid == nullptr || submittedSsid[0] == '\0')) return NetworkEntryError::None;
    // An explicit open network, or a supplied passphrase (length checked by validateCredentials), is fine.
    if (openNetwork) return NetworkEntryError::None;
    if (submittedPass != nullptr && submittedPass[0] != '\0') return NetworkEntryError::None;
    // A network is being set with no passphrase and Open unticked: the silent-open brick (ADR-0106).
    return NetworkEntryError::PassphraseRequired;
}

uint16_t parseDelaySeconds(const char* text) {
    if (text == nullptr) return 0;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (value < 0) return 0;                 // negative/non-numeric -> off, never a wrapped huge value.
    if (value > 65535) return 65535;         // over-range -> capped, never wrapped (client max is advisory).
    return static_cast<uint16_t>(value);
}

}  // namespace sapper
