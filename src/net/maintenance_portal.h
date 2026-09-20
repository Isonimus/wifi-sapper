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

#include "net/cracked_manifest.h"    // CrackedManifest — the §12 read seam the dashboard enumerates.
#include "net/provisioning.h"        // kSoftApSsidBufSize, formatSoftApSsid, isUsableMaintenancePass
#include "net/provisioning_store.h"  // ProvisioningRecord — the record the config form renders/merges.

namespace sapper {

class MaintenancePortal {
public:
    /**
     * @brief Construct the Maintenance server over the (already begun) manifest and the loaded record.
     * @param manifest         The loaded results mirror; enumerated read-only for the dashboard rows.
     * @param creds            Caller-owned, outlives the portal (runMaintenance never returns). Its
     *                         deauth arm feeds the summary, its maintenancePass hardens the AP, and its
     *                         stored fields drive the config form (ADR-0043) — the loadProvisioning()
     *                         result injected once (§4 #6), not re-read per request.
     * @param captureQueueDepth Handshakes persisted awaiting upload, shown in the summary.
     */
    MaintenancePortal(const CrackedManifest& manifest, const ProvisioningRecord& creds,
                      size_t captureQueueDepth)
        : m_manifest(manifest), m_creds(creds), m_queueDepth(captureQueueDepth) {}

    /// Bring up the hardened SoftAP, DNS hijack, and dashboard + control server.
    /// @return true once the AP and server are listening; false if the SoftAP failed to start.
    bool begin();

    /// Pump DNS and HTTP once. Call every Maintenance-loop iteration, alongside the serial pump.
    void handle();

    /// True if a request was served since the last call, and clears the flag — the caller uses it to
    /// push the no-activity backstop forward (ADR-0039 decision 7) so an in-use dashboard stays up.
    bool consumeActivity();

    /// True once a control (resume or a persisted re-provision) has asked to reboot into Station. The
    /// caller reboots *after* the HTTP response has flushed, never inside the handler (ADR-0043 #23), so
    /// the operator sees the acknowledgement before the AP drops.
    bool rebootRequested() const { return m_pendingReboot; }

    /// The SoftAP SSID being broadcast ("Sapper-XXXX"), for the boot banner and the panel screen.
    const char* apSsid() const { return m_ssid; }

private:
    void handleRoot();
    void handleConfig();   // GET /config — the setup form, rendered from the stored record (no echo, §4 #20).
    void handleSave();     // POST /save — merge + persist (§4 #6), then request a reboot into Station.
    void handleResume();   // POST /resume — request a reboot into Station (which syncs on its first window).

    const CrackedManifest& m_manifest;
    const ProvisioningRecord& m_creds;
    size_t m_queueDepth;
    bool m_activity = false;
    bool m_pendingReboot = false;

    DNSServer m_dns;
    WebServer m_http{80};
    char m_ssid[kSoftApSsidBufSize] = {0};
};

}  // namespace sapper

#endif  // UNIT_TEST
