/**
 * @file webhook_transport_esp32.cpp
 * @brief HTTPS POST to the configured push webhook (ADR-0023). Device-only.
 */
#include "surface/webhook_transport_esp32.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

namespace sapper {
namespace {

constexpr uint32_t kTlsTimeoutSec = 15;
constexpr uint32_t kHttpTimeoutMs = 15000;

// The ESP-IDF default full Mozilla root-CA bundle, embedded in the framework's libmbedtls.a
// (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y). Referencing the already-linked blob costs no
// extra flash; setCACertBundle() below makes WiFiClientSecure verify against it (ADR-0023 decision 5).
extern const uint8_t kRootCaBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t kRootCaBundleEnd[] asm("_binary_x509_crt_bundle_end");

}  // namespace

bool Esp32WebhookTransport::post(const WebhookRequest& request) {
    WiFiClientSecure client;
    // Validate the peer against the standard root store — arbitrary host, so a bundle not a pin.
    // kRootCaBundleEnd/Start are two linker symbols bracketing one embedded blob; end - start is its
    // byte length. cppcheck flags the cross-symbol subtraction (comparePointers), but this is the
    // standard, correct idiom for sizing a linker-embedded binary — there is no other way to get it
    // (ADR-0051).
    // cppcheck-suppress comparePointers
    const size_t bundleLen = static_cast<size_t>(kRootCaBundleEnd - kRootCaBundleStart);
    client.setCACertBundle(kRootCaBundleStart, bundleLen);
    client.setTimeout(kTlsTimeoutSec);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, request.url)) {
        // Never log request.url — it carries the operator's ntfy topic / Discord token (a secret).
        Serial.println("[ERROR] webhook HTTPClient begin failed (bad url?)");
        return false;
    }
    if (request.contentType != nullptr) http.addHeader("Content-Type", request.contentType);
    if (request.title != nullptr) {
        http.addHeader("Title", request.title);   // ntfy notification title.
        http.addHeader("Priority", "high");        // a recovered password is worth a loud push.
    }

    const size_t bodyLen = std::strlen(request.body);
    // HTTPClient::POST takes a non-const payload pointer but does not modify it; the body is our own
    // scratch buffer, so the const_cast is safe.
    const int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(request.body)), bodyLen);
    http.end();

    const bool ok = code >= 200 && code < 300;  // any 2xx (ntfy 200, Discord 204) is success.
    if (ok) {
        Serial.printf("[WEBHOOK] sent code=%d bytes=%u\n", code, static_cast<unsigned>(bodyLen));
    } else {
        // A negative code is an HTTPClient/TLS error (incl. a cert that chains to no trusted root); a
        // positive non-2xx is the server rejecting. Loud either way; the notifier keeps and retries.
        Serial.printf("[ERROR] webhook POST failed code=%d\n", code);
    }
    return ok;
}

}  // namespace sapper

#endif  // UNIT_TEST
