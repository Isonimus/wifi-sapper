/**
 * @file uploader_wpasec.cpp
 * @brief wpa-sec TLS upload of one in-RAM pcap (ADR-0017 decisions #1, #2). Device-only.
 */
#include "net/uploader_wpasec.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <memory>
#include <new>

#include "net/wpasec_response.h"
#include "net/wpasec_root_ca.h"

namespace sapper {
namespace {

constexpr const char* kWpaSecHost = "wpa-sec.stanev.org";
constexpr uint16_t kWpaSecPort = 443;
constexpr const char* kBoundary = "----WifiSapperBoundary";
constexpr uint32_t kTlsTimeoutMs = 15000;
constexpr uint32_t kResponseIdleTimeoutMs = 5000;
// wpa-sec returns hcxpcapngtool's verbose summary — a few KB, chunked. Buffer it whole (bounded) on
// the heap, not the loop-task stack, then hand it to the pure, host-tested classifier. If a response
// ever exceeds the bound it is truncated, which can only *lose* a success signal (→ Rejected → retry),
// never invent one — the parse stays fail-closed (quality bar §3).
constexpr size_t kMaxResponseLen = 16384;

/// Read the full response off TLS into a heap buffer and classify it (net/wpasec_response.h).
UploadResult readAndClassify(WiFiClientSecure& client, size_t uploadLen) {
    std::unique_ptr<char[]> buf(new (std::nothrow) char[kMaxResponseLen]);
    if (!buf) {
        Serial.println("[ERROR] wpa-sec response buffer alloc failed");
        return UploadResult::Rejected;  // cannot read the reply — fail closed and retry.
    }
    size_t n = 0;
    uint32_t lastMs = millis();
    while ((client.connected() || client.available()) && n < kMaxResponseLen - 1) {
        if (client.available()) {
            buf[n++] = static_cast<char>(client.read());
            lastMs = millis();
        } else {
            if (millis() - lastMs > kResponseIdleTimeoutMs) break;
            delay(10);
        }
    }
    buf[n] = '\0';
#ifdef SAPPER_TEST_HOOKS
    Serial.printf("[UPLOAD] resp uploadBytes=%u respBytes=%u\n", static_cast<unsigned>(uploadLen),
                  static_cast<unsigned>(n));
#endif
    return classifyWpaSecResponse(buf.get());
}

}  // namespace

UploadResult WpaSecUploader::upload(const uint8_t* pcap, size_t len, const char* key) {
    WiFiClientSecure client;
    client.setCACert(kWpaSecRootCaPem);  // the single pinned trust anchor (ADR-0017 decision #1).
    client.setTimeout(kTlsTimeoutMs / 1000);

    if (!client.connect(kWpaSecHost, kWpaSecPort)) {
        // A TLS/connect failure (including a cert that does not chain to the pinned root) is loud and
        // retryable — never a silent fallback that would leak the key/handshake (decision #1).
        Serial.println("[ERROR] wpa-sec TLS connect failed (cert pin or network)");
        return UploadResult::Rejected;
    }

    // Multipart body: the pcap in the `file` field. The filename is cosmetic — wpa-sec reads the SSID
    // from the beacon inside the pcap, not the name.
    char header[160];
    const int headerLen =
        std::snprintf(header, sizeof(header),
                      "--%s\r\nContent-Disposition: form-data; name=\"file\"; "
                      "filename=\"capture.pcap\"\r\nContent-Type: application/octet-stream\r\n\r\n",
                      kBoundary);
    char footer[48];
    const int footerLen = std::snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", kBoundary);
    const size_t contentLength = static_cast<size_t>(headerLen) + len + static_cast<size_t>(footerLen);

    client.printf("POST / HTTP/1.1\r\n");
    client.printf("Host: %s\r\n", kWpaSecHost);
    client.printf("Cookie: key=%s\r\n", key);  // wpa-sec authenticates the upload by this cookie.
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", kBoundary);
    client.printf("Content-Length: %u\r\n", static_cast<unsigned>(contentLength));
    client.printf("Connection: close\r\n\r\n");

    client.write(reinterpret_cast<const uint8_t*>(header), static_cast<size_t>(headerLen));
    client.write(pcap, len);
    client.write(reinterpret_cast<const uint8_t*>(footer), static_cast<size_t>(footerLen));

    const UploadResult result = readAndClassify(client, len);
    client.stop();

    const char* label = result == UploadResult::Accepted   ? "accepted"
                        : result == UploadResult::Duplicate ? "duplicate"
                                                            : "rejected";
    Serial.printf("[UPLOAD] wpa-sec result=%s bytes=%u\n", label, static_cast<unsigned>(len));
    return result;
}

}  // namespace sapper

#endif  // UNIT_TEST
