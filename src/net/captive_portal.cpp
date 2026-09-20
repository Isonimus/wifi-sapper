/**
 * @file captive_portal.cpp
 * @brief First-boot provisioning portal (ADR-0006). Device-only.
 */
#ifndef UNIT_TEST

#include "net/captive_portal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include <cstring>

#include "net/provisioning_form.h"  // SetupFormModel, buildSetupForm, resolveProvisioningUpdate

namespace sapper {
namespace {

// DNS hijack: answer every name with the AP IP so any hostname the client resolves lands on the
// form, which is what triggers the OS captive-portal check (ADR-0006 #1).
constexpr uint8_t kDnsPort = 53;
constexpr char kDnsWildcard[] = "*";

constexpr char kSuccessHtml[] =
    "<!DOCTYPE html><html><body><h2>Saved.</h2>"
    "<p>The Sapper is rebooting to join your network.</p></body></html>";

/**
 * @brief Copy a form field into a fixed buffer, rejecting an over-length value instead of
 *        truncating it. Truncation would let a too-long SSID slip past validateCredentials as a
 *        different, shorter network — so a value that does not fit is a hard failure here.
 * @return false if @p src is too long to fit @p dst with its terminator.
 */
bool copyBounded(char* dst, size_t dstSize, const String& src) {
    if (src.length() >= dstSize) {
        return false;
    }
    std::memcpy(dst, src.c_str(), src.length() + 1);  // includes the NUL
    return true;
}

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
        model.notifyCaptured = stored.notifyCaptured;
        model.notifyCracked = stored.notifyCracked;
        model.notifySyncError = stored.notifySyncError;
        model.hasStoredKey = stored.key[0] != '\0';
        model.hasStoredWebhook = stored.webhookUrl[0] != '\0';
        model.hasStoredMaintenancePass = stored.maintenancePass[0] != '\0';
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
    ProvisioningRecord submitted = {};
    // Over-length any field -> reject as a bad submission rather than store a truncated value.
    if (!copyBounded(submitted.ssid, sizeof(submitted.ssid), m_http.arg("ssid")) ||
        !copyBounded(submitted.pass, sizeof(submitted.pass), m_http.arg("pass")) ||
        !copyBounded(submitted.key, sizeof(submitted.key), m_http.arg("key")) ||
        !copyBounded(submitted.webhookUrl, sizeof(submitted.webhookUrl), m_http.arg("webhook")) ||
        !copyBounded(submitted.maintenancePass, sizeof(submitted.maintenancePass),
                     m_http.arg("maintpass"))) {
        m_http.send(400, "text/html", "<h2>A field is too long.</h2><p><a href='/'>Back</a></p>");
        return;
    }
    // Arm deauth only if the operator actively checked the box (ADR-0029): an HTML checkbox posts
    // "on" when checked and is absent otherwise, so an unset field is the default-off disarmed state.
    // The form now renders the stored state (ADR-0037), so an unchecked box is a deliberate disarm.
    submitted.deauthEnabled = (m_http.arg("deauth") == "on");
    // Per-type push selector (ADR-0035, §4 #19): each box posts "on" when checked, absent otherwise.
    submitted.notifyCaptured = (m_http.arg("nCaptured") == "on");
    submitted.notifyCracked = (m_http.arg("nCracked") == "on");
    submitted.notifySyncError = (m_http.arg("nSyncErr") == "on");

    // Merge over the stored record (ADR-0037): a blank key/webhook keeps its stored value, so a re-save
    // to fix WiFi does not silently wipe the secret the operator did not retype; the pair and the
    // toggles always come from this submission. First boot (no stored record) is a verbatim copy.
    ProvisioningRecord stored = {};
    const bool hasStored = loadProvisioning(stored);
    ProvisioningRecord record = {};
    resolveProvisioningUpdate(submitted, stored, hasStored, record);

    if (validateCredentials(record.ssid, record.pass, record.key) != CredentialError::None) {
        m_http.send(400, "text/html", "<h2>Invalid credentials.</h2><p><a href='/'>Back</a></p>");
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
