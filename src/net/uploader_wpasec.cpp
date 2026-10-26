/**
 * @file uploader_wpasec.cpp
 * @brief wpa-sec TLS upload of one in-RAM pcap (ADR-0017 decisions #1, #2). Device-only.
 */
#include "net/uploader_wpasec.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include "net/wpasec_response.h"
#include "net/wpasec_root_ca.h"

namespace sapper {
namespace {

constexpr const char* kWpaSecHost = "wpa-sec.stanev.org";
constexpr uint16_t kWpaSecPort = 443;
constexpr const char* kBoundary = "----WifiSapperBoundary";
constexpr uint32_t kTlsTimeoutMs = 15000;
constexpr uint32_t kResponseIdleTimeoutMs = 5000;
// The reply is classified by streaming (ADR-0076, §4 #35), not buffered whole: the former 16KB `new`
// failed on the no-PSRAM S3 under TLS memory pressure (largest contiguous block measured at 7668 B), so
// every reply misclassified Rejected and no capture was ever marked submitted. Read the TLS bytes into
// this small fixed stack buffer and feed them to the pure, host-tested classifier chunk by chunk — no heap
// allocation sized to the response.
constexpr size_t kResponseChunkLen = 256;
// Hard ceiling on bytes read, so a server trickling bytes forever cannot hold this synchronous upload open
// indefinitely (the per-gap idle timeout alone would not bound a slow-drip stream). Far above the measured
// ~4.3 KB reply, so a real response is never truncated; hitting it just stops the read and classifies what
// arrived (fail-closed — Rejected and retry if the decisive token had not yet been seen).
constexpr size_t kMaxResponseBytes = 32768;

/// Stream the response off TLS through the pure classifier (net/wpasec_response.h) — no heap alloc.
UploadResult readAndClassify(WiFiClientSecure& client, size_t uploadLen) {
    WpaSecResponseClassifier classifier;
    size_t n = 0;
    uint32_t lastMs = millis();
    char chunk[kResponseChunkLen];
    // Stop early once the verdict is terminal (a duplicate); otherwise read to the end so a late dedup
    // line still outranks an earlier positive count (ADR-0076 #2).
    while ((client.connected() || client.available()) && !classifier.terminal() && n < kMaxResponseBytes) {
        const int r =
            client.available() ? client.read(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk)) : 0;
        if (r > 0) {
            classifier.feed(chunk, static_cast<size_t>(r));
            n += static_cast<size_t>(r);
            lastMs = millis();
        } else {
            // No bytes available, or read() returned 0/<0 (a transient TLS condition): advance toward the
            // idle timeout and yield — never busy-spin, which would starve the task watchdog and reboot.
            if (millis() - lastMs > kResponseIdleTimeoutMs) break;
            delay(10);
        }
    }
#ifdef SAPPER_TEST_HOOKS
    Serial.printf("[UPLOAD] resp uploadBytes=%u respBytes=%u\n", static_cast<unsigned>(uploadLen),
                  static_cast<unsigned>(n));
#endif
    return classifier.result();
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
