/**
 * @file captive_portal.h
 * @brief First-boot provisioning portal: SoftAP + DNS hijack + one config form (ADR-0006).
 *
 * Device-only (SoftAP, DNSServer, WebServer); excluded from the native lane and proven on
 * hardware by the slice-0007 verify script (Scenario C). It reuses the DNS+WebServer *shape* of
 * the Adversary's `ap::ArduinoCaptivePortal` — pointerless members, DNS wildcard to the AP IP,
 * not-found serves the page so the OS captive check pops the form — but NONE of its credential
 * harvesting: this portal collects the operator's OWN network + wpa-sec key and hands them
 * straight to `persistProvisioning()`. It never stores or forwards a third party's credentials.
 *
 * Provisioning is not a serial actuation path (CLAUDE.md §4 invariant #7): the only way to write
 * credentials is a human POST to this form (or the compile-time `SAPPER_TEST_HOOKS` seed) — never
 * a serial command.
 */
#pragma once

#ifndef UNIT_TEST

#include <DNSServer.h>
#include <WebServer.h>

#include "net/provisioning.h"        // kSoftApSsidBufSize, formatSoftApSsid, validateCredentials
#include "net/provisioning_store.h"  // ProvisioningRecord, persistProvisioning

namespace sapper {

/// The default SoftAP passphrase, documented in the README so a screenless operator can join
/// (ADR-0006 #5). WPA2 needs >= 8 chars; kept fixed and public rather than random so the setup
/// step is reproducible from the docs. The AP is unattended for seconds during setup only.
constexpr char kSoftApPassword[] = "sapper-setup";

class CaptivePortal {
public:
    /**
     * @brief Bring up the SoftAP, DNS hijack, and config form.
     * @return true once AP and servers are listening; false if the SoftAP failed to start.
     *
     * The SSID ("Sapper-XXXX") is derived from the SoftAP MAC read from efuse here — not from a
     * caller-supplied MAC, because the WiFi-interface MAC reads back as zeros until WiFi has been
     * started, which would name every unit "Sapper-0000".
     */
    bool begin();

    /// Pump DNS and HTTP once. Must be called every provisioning-loop iteration, alongside the
    /// serial channel pump, so the pre-engine portal state stays responsive (invariant #3).
    void handle();

    /// True once a valid form submission has been persisted; the caller then reboots into STA.
    bool provisioned() const { return m_provisioned; }

    /// The SoftAP SSID this portal is broadcasting ("Sapper-XXXX"), for the boot banner.
    const char* apSsid() const { return m_ssid; }

private:
    void handleRoot();
    void handleSave();

    DNSServer m_dns;
    WebServer m_http{80};
    char m_ssid[kSoftApSsidBufSize] = {0};
    bool m_provisioned = false;
};

}  // namespace sapper

#endif  // UNIT_TEST
