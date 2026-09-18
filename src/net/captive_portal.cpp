/**
 * @file captive_portal.cpp
 * @brief First-boot provisioning portal (ADR-0006). Device-only.
 */
#ifndef UNIT_TEST

#include "net/captive_portal.h"

#include <WiFi.h>
#include <esp_mac.h>

#include <cstring>

namespace sapper {
namespace {

// DNS hijack: answer every name with the AP IP so any hostname the client resolves lands on the
// form, which is what triggers the OS captive-portal check (ADR-0006 #1).
constexpr uint8_t kDnsPort = 53;
constexpr char kDnsWildcard[] = "*";

// The one config form. Static and value-free: it carries no device state, so it needs no
// per-request rendering. Empty passphrase = an open network (validateCredentials allows it).
constexpr char kFormHtml[] =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Sapper setup</title></head><body>"
    "<h2>WiFi Sapper setup</h2>"
    "<form method='POST' action='/save'>"
    "<p>Network name (SSID)<br><input name='ssid' maxlength='32' required></p>"
    "<p>Passphrase (blank for open)<br><input name='pass' type='password' maxlength='63'></p>"
    "<p>wpa-sec API key<br><input name='key' maxlength='64' required></p>"
    "<p><button type='submit'>Save &amp; reboot</button></p>"
    "</form></body></html>";

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

void CaptivePortal::handleRoot() { m_http.send(200, "text/html", kFormHtml); }

void CaptivePortal::handleSave() {
    ProvisioningRecord record = {};
    // Over-length any field -> reject as a bad submission rather than store a truncated value.
    if (!copyBounded(record.ssid, sizeof(record.ssid), m_http.arg("ssid")) ||
        !copyBounded(record.pass, sizeof(record.pass), m_http.arg("pass")) ||
        !copyBounded(record.key, sizeof(record.key), m_http.arg("key"))) {
        m_http.send(400, "text/html", "<h2>A field is too long.</h2><p><a href='/'>Back</a></p>");
        return;
    }

    if (validateCredentials(record.ssid, record.pass, record.key) != CredentialError::None) {
        m_http.send(400, "text/html", "<h2>Invalid credentials.</h2><p><a href='/'>Back</a></p>");
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
