/**
 * @file maintenance_portal.cpp
 * @brief The Maintenance-phase dashboard SoftAP (ADR-0039). Device-only.
 */
#ifndef UNIT_TEST

#include "net/maintenance_portal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include <cstring>

#include "net/captive_portal.h"      // kSoftApPassword — the published fallback AP passphrase.
#include "net/dashboard.h"           // buildDashboardHead, appendCrackedRow, kDashboardControls/Foot
#include "net/provisioning_form.h"   // SetupFormModel, buildSetupForm, resolveProvisioningUpdate

namespace sapper {
namespace {

constexpr uint8_t kDnsPort = 53;
constexpr char kDnsWildcard[] = "*";

constexpr char kResumeHtml[] =
    "<!DOCTYPE html><html><body><h2>Resuming.</h2>"
    "<p>The Sapper is rebooting to hunt.</p></body></html>";

constexpr char kSavedHtml[] =
    "<!DOCTYPE html><html><body><h2>Saved.</h2>"
    "<p>The Sapper is rebooting to hunt with the new configuration.</p></body></html>";

/// Choose the Maintenance AP passphrase (ADR-0039 decision 6): the operator's value when it is a usable
/// non-empty WPA2 passphrase, else the published default. An invalid stored value (which the portal
/// save-path rejects) also falls back rather than being handed to WiFi.softAP() to drop to an open AP.
const char* effectiveApPass(const char* maintenancePass) {
    if (maintenancePass != nullptr && maintenancePass[0] != '\0' &&
        isUsableMaintenancePass(maintenancePass)) {
        return maintenancePass;
    }
    return kSoftApPassword;
}

/// Copy a form field into a fixed buffer, rejecting an over-length value rather than truncating it (a
/// truncated SSID could slip past validateCredentials as a different network). Second instance of the
/// captive-portal helper — left duplicated per the rule of three (§3), see LEDGER (ADR-0043 decision 6).
bool copyBounded(char* dst, size_t dstSize, const String& src) {
    if (src.length() >= dstSize) return false;
    std::memcpy(dst, src.c_str(), src.length() + 1);  // includes the NUL
    return true;
}

}  // namespace

bool MaintenancePortal::begin() {
    // Read the SoftAP MAC from efuse (valid before WiFi starts, unlike the interface MAC), same as the
    // captive portal. The Maintenance AP carries a DISTINCT "Sapper-Maint-XXXX" identity (ADR-0055) so it
    // is never mistaken for the provisioning portal's "Sapper-XXXX" (e.g. the STA-fail fallback).
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    formatMaintenanceApSsid(mac, m_ssid);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(m_ssid, effectiveApPass(m_creds.maintenancePass))) {
        return false;  // fail loud: no AP means no dashboard — the caller reports [FATAL].
    }
    const IPAddress apIp = WiFi.softAPIP();
    m_dns.start(kDnsPort, kDnsWildcard, apIp);

    m_http.on("/", HTTP_GET, [this]() { handleRoot(); });
    m_http.on("/config", HTTP_GET, [this]() { handleConfig(); });
    // The two mutating controls are POST only (ADR-0043 decision 3, §4 #23): a GET reaches only the
    // read pages, so an OS captive-check probe or a browser prefetch cannot reboot or re-provision.
    m_http.on("/resume", HTTP_POST, [this]() { handleResume(); });
    m_http.on("/save", HTTP_POST, [this]() { handleSave(); });
    // Any other path (OS captive-check URLs included) also serves the dashboard, so opening a browser
    // lands on it directly. GET only by construction — onNotFound covers all methods, but the mutating
    // routes above are registered HTTP_POST, so an unmatched GET falls here (a safe read).
    m_http.onNotFound([this]() { handleRoot(); });
    m_http.begin();
    return true;
}

void MaintenancePortal::handle() {
    m_dns.processNextRequest();
    m_http.handleClient();
}

bool MaintenancePortal::consumeActivity() {
    const bool served = m_activity;
    m_activity = false;
    return served;
}

void MaintenancePortal::handleRoot() {
    m_activity = true;  // a request keeps the dashboard alive against the no-activity backstop.

    DashboardStats stats;
    stats.recoveredCount = m_manifest.size();
    stats.captureQueueDepth = m_queueDepth;
    stats.deauthArmed = m_creds.deauthEnabled;
    stats.everSynced = !m_manifest.isFresh();  // the persisted fresh flag, not a RAM last-sync status.

    char head[kDashboardHeadBufSize];
    if (buildDashboardHead(stats, head, sizeof(head)) == 0) {
        // The head outgrew its buffer (a bug, not data — the rows stream separately): serve an error,
        // never a truncated page (§3, fail loud).
        m_http.send(500, "text/html", "<h2>Could not render the dashboard.</h2>");
        return;
    }

    // Stream the page: chunked so the up-to-256-entry account is never held whole in RAM (dashboard.h).
    m_http.setContentLength(CONTENT_LENGTH_UNKNOWN);
    m_http.send(200, "text/html", head);

    char row[kDashboardRowBufSize];
    for (size_t i = 0; i < m_manifest.size(); ++i) {
        const CrackedEntry* entry = m_manifest.entryAt(i);
        if (entry == nullptr) break;  // count changed under us: stop rather than read a stale slot.
        if (appendCrackedRow(entry->result, row, sizeof(row)) == 0) {
            // Unreachable for a valid CrackedResult (the row buffer is sized for the worst case), so a 0
            // here is a bug: surface it on serial and skip the row rather than corrupt the stream.
            Serial.println("[MAINT] row render overflow — skipping an entry");
            continue;
        }
        m_http.sendContent(row);
    }
    m_http.sendContent(kDashboardControls);  // closes the table, then the resume form + /config link.
    m_http.sendContent(kDashboardFoot);      // closes the document.
}

void MaintenancePortal::handleConfig() {
    m_activity = true;  // opening the config form keeps the dashboard alive against the backstop.

    // Render the setup form from the *stored* record so the armed/notify toggles show their real state
    // and a re-save does not silently reset them (ADR-0037). The model carries no secret string, so no
    // provisioned secret can reach the served HTML (§4 #20) — only the "a secret is stored" flags. The
    // record was loaded once via loadProvisioning() (§4 #6) and injected; Maintenance is entered only for
    // a provisioned device (ADR-0039 decision 2), so there is always a stored record here.
    SetupFormModel model = {};
    model.deauthArmed = m_creds.deauthEnabled;
    model.notifyCaptured = m_creds.notifyCaptured;
    model.notifyCracked = m_creds.notifyCracked;
    model.notifySyncError = m_creds.notifySyncError;
    model.hasStoredKey = m_creds.key[0] != '\0';
    model.hasStoredWebhook = m_creds.webhookUrl[0] != '\0';
    model.hasStoredMaintenancePass = m_creds.maintenancePass[0] != '\0';

    char html[kSetupFormBufSize];
    if (buildSetupForm(model, html, sizeof(html)) == 0) {
        m_http.send(500, "text/html", "<h2>Could not render the setup form.</h2>");  // fail loud (§3).
        return;
    }
    m_http.send(200, "text/html", html);
}

void MaintenancePortal::handleSave() {
    m_activity = true;

    // Second instance of the captive-portal save wiring — left duplicated per the rule of three (§3),
    // sharing the *pure* policy (resolveProvisioningUpdate) but repeating the WebServer::arg plumbing;
    // see LEDGER (ADR-0043 decision 6). Over-length any field -> reject rather than store a truncation.
    ProvisioningRecord submitted = {};
    if (!copyBounded(submitted.ssid, sizeof(submitted.ssid), m_http.arg("ssid")) ||
        !copyBounded(submitted.pass, sizeof(submitted.pass), m_http.arg("pass")) ||
        !copyBounded(submitted.key, sizeof(submitted.key), m_http.arg("key")) ||
        !copyBounded(submitted.webhookUrl, sizeof(submitted.webhookUrl), m_http.arg("webhook")) ||
        !copyBounded(submitted.maintenancePass, sizeof(submitted.maintenancePass),
                     m_http.arg("maintpass"))) {
        m_http.send(400, "text/html", "<h2>A field is too long.</h2><p><a href='/config'>Back</a></p>");
        return;
    }
    // An HTML checkbox posts "on" when checked and is absent otherwise; an unchecked box is a deliberate
    // disarm now the form renders the stored state (ADR-0029/ADR-0035/ADR-0037).
    submitted.deauthEnabled = (m_http.arg("deauth") == "on");
    submitted.notifyCaptured = (m_http.arg("nCaptured") == "on");
    submitted.notifyCracked = (m_http.arg("nCracked") == "on");
    submitted.notifySyncError = (m_http.arg("nSyncErr") == "on");

    // Merge over the stored record (ADR-0037): a blank key/webhook/maintpass keeps its stored value, the
    // SSID/pass pair and the toggles come from the submission. Maintenance always has a stored record.
    ProvisioningRecord record = {};
    resolveProvisioningUpdate(submitted, m_creds, /*hasStored=*/true, record);

    if (validateCredentials(record.ssid, record.pass, record.key) != CredentialError::None) {
        m_http.send(400, "text/html", "<h2>Invalid credentials.</h2><p><a href='/config'>Back</a></p>");
        return;
    }
    if (record.webhookUrl[0] != '\0' && !isUsableWebhookUrl(record.webhookUrl)) {
        m_http.send(400, "text/html",
                    "<h2>Webhook URL must be an https:// address.</h2><p><a href='/config'>Back</a></p>");
        return;
    }
    if (!isUsableMaintenancePass(record.maintenancePass)) {
        m_http.send(400, "text/html",
                    "<h2>Maintenance passphrase must be at least 8 characters.</h2>"
                    "<p><a href='/config'>Back</a></p>");
        return;
    }
    if (!persistProvisioning(record)) {
        // The store validated the same triad, so a failure here is NVS, not the input — fail loud (§3).
        m_http.send(500, "text/html", "<h2>Could not save to storage.</h2>");
        return;
    }

    m_http.send(200, "text/html", kSavedHtml);
    m_pendingReboot = true;  // reboot into Station after the response flushes (the loop does it, §4 #23).
}

void MaintenancePortal::handleResume() {
    m_activity = true;
    m_http.send(200, "text/html", kResumeHtml);
    // A reboot-scoped intent, not a live engine action (ADR-0043 #23): a clean power-on without BOOT
    // re-enters Station via the boot gate, and its always-due scheduler syncs on the first window.
    m_pendingReboot = true;
}

}  // namespace sapper

#endif  // UNIT_TEST
