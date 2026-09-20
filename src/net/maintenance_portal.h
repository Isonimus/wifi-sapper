/**
 * @file maintenance_portal.h
 * @brief The Maintenance-phase SoftAP that serves the read-only results dashboard (ADR-0039).
 *
 * Device-only (SoftAP, DNSServer, WebServer); excluded from the native lane and proven on hardware by
 * scripts/0040-maintenance-verify.mjs (slice-0040 Scenario G). It reuses the captive portal's shape —
 * SoftAP + DNS wildcard + WebServer, not-found serves the page — but for a different job: no hunting
 * runs in this boot phase, so it serves the persisted recovered results the operator held BOOT to read.
 *
 * The page is rendered by the pure net/dashboard functions and streamed chunked (WebServer::sendContent)
 * so the up-to-256-entry, ~36 KB account is never materialised in one buffer (dashboard.h). Cracked
 * passwords are read only through the injected CrackedManifest seam (§4 invariant #12) — this class
 * never parses the store itself. The AP is hardened by an operator-set passphrase (ADR-0039 decision 6):
 * an empty/invalid one falls back to the published kSoftApPassword.
 */
#pragma once

#ifndef UNIT_TEST

#include <DNSServer.h>
#include <WebServer.h>

#include "net/cracked_manifest.h"  // CrackedManifest — the §12 read seam the dashboard enumerates.
#include "net/provisioning.h"      // kSoftApSsidBufSize, formatSoftApSsid, isUsableMaintenancePass

namespace sapper {

class MaintenancePortal {
public:
    /**
     * @brief Construct the Maintenance server over the (already begun) manifest and the persisted stats.
     * @param manifest         The loaded results mirror; enumerated read-only for the dashboard rows.
     * @param deauthArmed      Persisted arm state (ADR-0029), shown in the summary.
     * @param captureQueueDepth Handshakes persisted awaiting upload, shown in the summary.
     * @param maintenancePass  Caller-owned; the hardened AP passphrase. Empty or invalid (per
     *                         isUsableMaintenancePass) → the AP falls back to the published default.
     */
    MaintenancePortal(const CrackedManifest& manifest, bool deauthArmed, size_t captureQueueDepth,
                      const char* maintenancePass)
        : m_manifest(manifest),
          m_deauthArmed(deauthArmed),
          m_queueDepth(captureQueueDepth),
          m_maintenancePass(maintenancePass) {}

    /// Bring up the hardened SoftAP, DNS hijack, and dashboard server.
    /// @return true once the AP and server are listening; false if the SoftAP failed to start.
    bool begin();

    /// Pump DNS and HTTP once. Call every Maintenance-loop iteration, alongside the serial pump.
    void handle();

    /// True if a request was served since the last call, and clears the flag — the caller uses it to
    /// push the no-activity backstop forward (ADR-0039 decision 7) so an in-use dashboard stays up.
    bool consumeActivity();

    /// The SoftAP SSID being broadcast ("Sapper-XXXX"), for the boot banner and the panel screen.
    const char* apSsid() const { return m_ssid; }

private:
    void handleRoot();

    const CrackedManifest& m_manifest;
    bool m_deauthArmed;
    size_t m_queueDepth;
    const char* m_maintenancePass;
    bool m_activity = false;

    DNSServer m_dns;
    WebServer m_http{80};
    char m_ssid[kSoftApSsidBufSize] = {0};
};

}  // namespace sapper

#endif  // UNIT_TEST
