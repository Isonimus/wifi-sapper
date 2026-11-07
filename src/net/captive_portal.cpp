/**
 * @file captive_portal.cpp
 * @brief First-boot provisioning portal (ADR-0006). Device-only.
 */
#ifndef UNIT_TEST

#include "net/captive_portal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include "net/provisioning_form.h"  // SetupFormModel, buildSetupForm, resolveProvisioningUpdate, parse
#include "net/web_arg_reader.h"     // WebServerArgReader — the device FormArgReader adapter (ADR-0114)

namespace sapper {
namespace {

// DNS hijack: answer every name with the AP IP so any hostname the client resolves lands on the
// form, which is what triggers the OS captive-portal check (ADR-0006 #1).
constexpr uint8_t kDnsPort = 53;
constexpr char kDnsWildcard[] = "*";

constexpr char kSuccessHtml[] =
    "<!DOCTYPE html><html><body><h2>Saved.</h2>"
    "<p>The Sapper is rebooting to join your network.</p></body></html>";

}  // namespace

bool CaptivePortal::begin() {
    // Read the SoftAP MAC from efuse: valid before WiFi starts, unlike the interface MAC which
    // reads as zeros until then (which named the AP "Sapper-0000" on hardware).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    formatSoftApSsid(mac, m_ssid);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(m_ssid, kSoftApPassword)) {
        return false;  // fail loud: no AP means no way to provision — the caller reports [FATAL].
    }
    const IPAddress apIp = WiFi.softAPIP();
    m_dns.start(kDnsPort, kDnsWildcard, apIp);

    m_http.on("/", HTTP_GET, [this]() { handleRoot(); });
    m_http.on("/save", HTTP_POST, [this]() { handleSave(); });
    // Any other path (the OS captive-check URLs) also gets the form, so the setup sheet pops.
    m_http.onNotFound([this]() { handleRoot(); });
    m_http.begin();
    return true;
}

void CaptivePortal::handle() {
    m_dns.processNextRequest();
    m_http.handleClient();
}

void CaptivePortal::handleRoot() {
    // Render the form from the *stored* state so a re-save (the STA-fail fallback) shows the armed
    // deauth / push-selector toggles rather than silently resetting them (ADR-0037). The model carries
    // no secret string, so no provisioned secret can reach the served HTML (§4 #20) — only the fact
    // that a key/webhook is stored, to invite "leave blank to keep".
    ProvisioningRecord stored = {};
    SetupFormModel model = {};
    if (loadProvisioning(stored)) {
        model.deauthArmed = stored.deauthEnabled;
        model.pmkidSolicit = stored.pmkidSolicit;  // the second active-TX arm (ADR-0110 #5d).
        model.notifyCaptured = stored.notifyCaptured;
        model.notifyCracked = stored.notifyCracked;
        model.notifySyncError = stored.notifySyncError;
        model.hasStoredNetwork = stored.ssid[0] != '\0';  // SSID blank -> keep current Wi-Fi (ADR-0106).
        model.hasStoredKey = stored.key[0] != '\0';
        model.hasStoredWebhook = stored.webhookUrl[0] != '\0';
        model.hasStoredMaintenancePass = stored.maintenancePass[0] != '\0';
        model.hasStoredPwncrackKey = stored.pwncrackKey[0] != '\0';  // pwncrack second backend (ADR-0109 #46).
        // Prefill the screen-dim delays (ADR-0093): config, not secrets, so render the stored value back
        // (the two number inputs carry value='N'), so a re-save preserves them instead of wiping to 0.
        model.screenDimAfterSec = stored.screenDimAfterSec;
        model.screenOffAfterSec = stored.screenOffAfterSec;
    } else {
        // First boot (or a corrupt/half-written record): fresh defaults — "cracked" pre-checked to match
        // the ADR-0035 NVS read-default, everything else off and no secret stored.
        model.notifyCracked = true;
    }

    char html[kSetupFormBufSize];
    if (buildSetupForm(model, html, sizeof(html)) == 0) {
        // The form outgrew its buffer: serve an error, never a truncated half-form (§3, fail loud).
        m_http.send(500, "text/html", "<h2>Could not render the setup form.</h2>");
        return;
    }
    m_http.send(200, "text/html", html);
}

void CaptivePortal::handleSave() {
    // Parse the whole submission through the one pure seam both portals share (ADR-0114): it reads every
    // field (strings length-rejected not truncated, checkboxes, the clamped dim delays) so neither
    // serving surface can drift the field list. Over-length any field -> reject the submission.
    WebServerArgReader args(m_http);
    ProvisioningRecord submitted = {};
    bool openNetwork = false;
    if (parseSetupFormSubmission(args, submitted, openNetwork) != FormParseError::None) {
        m_http.send(400, "text/html", "<h2>A field is too long.</h2><p><a href='/'>Back</a></p>");
        return;
    }

    // Merge over the stored record (ADR-0037/ADR-0106/ADR-0109): a blank key/webhook/maintpass/pwncrack
    // keeps its stored value and a blank SSID keeps the whole Wi-Fi pair, so a re-save to change one field
    // does not wipe the rest; the toggles always come from this submission. First boot (no stored record)
    // takes the submission, Open clearing the passphrase.
    ProvisioningRecord stored = {};
    const bool hasStored = loadProvisioning(stored);
    ProvisioningRecord record = {};
    resolveProvisioningUpdate(submitted, stored, hasStored, openNetwork, record);

    if (validateCredentials(record.ssid, record.pass, record.key) != CredentialError::None) {
        m_http.send(400, "text/html", "<h2>Invalid credentials.</h2><p><a href='/'>Back</a></p>");
        return;
    }
    // Reject a new network entered with no passphrase and Open unticked (ADR-0106) — a blank passphrase no
    // longer silently configures an open network, which bricked association on re-provision. Run after
    // validateCredentials (which owns the empty-SSID / empty-key / length messages, and accepts an empty
    // pass as open) so this guard is what finally catches the forgotten-passphrase case with its own hint.
    if (validateNetworkEntry(submitted.ssid, submitted.pass, openNetwork, hasStored) !=
        NetworkEntryError::None) {
        m_http.send(400, "text/html",
                    "<h2>Enter a passphrase for the network, or tick &quot;Open network&quot;.</h2>"
                    "<p><a href='/'>Back</a></p>");
        return;
    }
    // The webhook is optional (ADR-0023): blank stores as disabled. But a non-blank value that is not a
    // usable https endpoint is rejected loudly rather than silently saved-and-ignored — a mistyped http://
    // URL should tell the operator, not quietly leave push off.
    if (record.webhookUrl[0] != '\0' && !isUsableWebhookUrl(record.webhookUrl)) {
        m_http.send(400, "text/html",
                    "<h2>Webhook URL must be an https:// address.</h2><p><a href='/'>Back</a></p>");
        return;
    }
    // The Maintenance AP passphrase (ADR-0039 #6) is optional (blank = default AP password), but a
    // non-blank value must meet the WPA2 minimum — a 1..7-char value is refused loudly rather than
    // silently dropping the results-viewer AP to an open network. (After the merge, so a kept value is
    // re-validated too.)
    if (!isUsableMaintenancePass(record.maintenancePass)) {
        m_http.send(400, "text/html",
                    "<h2>Maintenance passphrase must be at least 8 characters.</h2>"
                    "<p><a href='/'>Back</a></p>");
        return;
    }
    if (!persistProvisioning(record)) {
        // The store validated the same triad, so a failure here is NVS, not the input — fail loud.
        m_http.send(500, "text/html", "<h2>Could not save to storage.</h2>");
        return;
    }

    m_http.send(200, "text/html", kSuccessHtml);
    // Signal the caller; it reboots after the response has flushed so the operator sees "Saved".
    m_provisioned = true;
}

}  // namespace sapper

#endif  // UNIT_TEST
