/**
 * @file maintenance_portal.cpp
 * @brief The Maintenance-phase dashboard SoftAP (ADR-0039). Device-only.
 */
#ifndef UNIT_TEST

#include "net/maintenance_portal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include "net/captive_portal.h"  // kSoftApPassword — the published fallback AP passphrase.
#include "net/dashboard.h"       // buildDashboardHead, appendCrackedRow, kDashboardFoot, DashboardStats

namespace sapper {
namespace {

constexpr uint8_t kDnsPort = 53;
constexpr char kDnsWildcard[] = "*";

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

}  // namespace

bool MaintenancePortal::begin() {
    // Read the SoftAP MAC from efuse (valid before WiFi starts, unlike the interface MAC), same as the
    // captive portal, so the Maintenance AP carries the same "Sapper-XXXX" identity the README documents.
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    formatSoftApSsid(mac, m_ssid);

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(m_ssid, effectiveApPass(m_maintenancePass))) {
        return false;  // fail loud: no AP means no dashboard — the caller reports [FATAL].
    }
    const IPAddress apIp = WiFi.softAPIP();
    m_dns.start(kDnsPort, kDnsWildcard, apIp);

    m_http.on("/", HTTP_GET, [this]() { handleRoot(); });
    // Any other path (OS captive-check URLs included) also serves the dashboard, so opening a browser
    // lands on it directly.
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
    stats.deauthArmed = m_deauthArmed;
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
    m_http.sendContent(kDashboardFoot);
}

}  // namespace sapper

#endif  // UNIT_TEST
