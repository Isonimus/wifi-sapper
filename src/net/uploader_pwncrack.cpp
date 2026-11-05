/**
 * @file uploader_pwncrack.cpp
 * @brief pwncrack TLS upload of one hc22000 line (ADR-0109). Device-only.
 *
 * Transport glue only: the Accepted/Rejected judgment lives in the pure, host-tested classifier
 * (net/pwncrack_response.h), exactly as the wpa-sec uploader delegates to WpaSecResponseClassifier. This
 * file connects, POSTs the multipart body, and reads the HTTP reply into a bounded body buffer to hand over.
 *
 * Two parts of the device leg are unconfirmed until the operator on-air run (LEDGER, the Cloudflare device
 * leg): the TLS trust anchor (pwncrack_root_ca.h reuses the GTS Root R4 the workstation probe saw) and
 * whether Cloudflare serves the reply chunked. Both are handled fail-closed: a handshake failure or a body
 * this reader cannot cleanly isolate classifies Rejected, so the capture is kept and retried, never falsely
 * marked submitted.
 */
#include "net/uploader_pwncrack.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "net/pwncrack_response.h"
#include "net/pwncrack_root_ca.h"

namespace sapper {
namespace {

constexpr const char* kPwncrackHost = "pwncrack.org";
constexpr const char* kPwncrackPath = "/upload_handshake";
constexpr uint16_t kPwncrackPort = 443;
constexpr const char* kBoundary = "----WifiSapperPwncrackBoundary";
constexpr uint32_t kTlsTimeoutMs = 15000;
constexpr uint32_t kResponseIdleTimeoutMs = 5000;
// Hard ceiling on bytes read while skipping headers, so a server trickling or a huge header block cannot
// hold this synchronous upload open forever (Cloudflare adds many cf-* / Set-Cookie headers). Generous.
constexpr size_t kMaxHeaderBytes = 16384;

// Blocking single-byte read with an idle timeout; returns the byte (0..255) or -1 on close/timeout. Yields
// on no-data so it never busy-spins the task watchdog into a reboot (mirrors uploader_wpasec's read loop).
int readByte(WiFiClientSecure& client, uint32_t& lastMs) {
    for (;;) {
        if (client.available()) {
            uint8_t b = 0;
            if (client.read(&b, 1) == 1) {
                lastMs = millis();
                return b;
            }
        }
        if (!client.connected() && !client.available()) return -1;
        if (millis() - lastMs > kResponseIdleTimeoutMs) return -1;
        delay(5);
    }
}

// Parse the numeric status code out of an HTTP status line like "HTTP/1.1 200 OK". Returns 0 if the line is
// not a recognisable status line (fail-closed: classify() rejects a 0 status).
int parseStatusCode(const char* line) {
    const char* space = std::strchr(line, ' ');
    if (space == nullptr) return 0;
    int code = 0;
    const char* p = space + 1;
    if (*p < '0' || *p > '9') return 0;
    for (int i = 0; i < 3 && *p >= '0' && *p <= '9'; ++i, ++p) code = code * 10 + (*p - '0');
    return code;
}

// Read the status code and the (bounded) response body off TLS. Reads the status line, skips headers to the
// blank-line separator with a rolling CRLFCRLF match, then reads the body into @p body up to @p bodyCap. On
// any failure to isolate the body (timeout, no separator within the header ceiling) returns a 0 status so the
// classifier rejects. A body longer than @p bodyCap is truncated at the cap; classifyPwncrackResponse then
// rejects it on the over-bound guard rather than parsing a partial object (fail-closed).
int readResponse(WiFiClientSecure& client, char* body, size_t bodyCap, size_t& bodyLen) {
    bodyLen = 0;

    uint32_t lastMs = millis();
    // Status line: accumulate until CR, then expect the LF.
    char statusLine[64];
    size_t sl = 0;
    for (;;) {
        const int c = readByte(client, lastMs);
        if (c < 0) return 0;
        if (c == '\r') { readByte(client, lastMs); break; }  // consume the paired LF.
        if (sl + 1 < sizeof(statusLine)) statusLine[sl++] = static_cast<char>(c);
    }
    statusLine[sl] = '\0';
    const int status = parseStatusCode(statusLine);
    if (status == 0) return 0;

    // Skip headers until the CRLFCRLF that ends them, watching a 4-byte rolling window.
    char window[4] = {0, 0, 0, 0};
    size_t headerBytes = 0;
    for (;;) {
        const int c = readByte(client, lastMs);
        if (c < 0) return 0;  // closed before the body separator: cannot isolate the body, fail closed.
        if (++headerBytes > kMaxHeaderBytes) return 0;
        window[0] = window[1];
        window[1] = window[2];
        window[2] = window[3];
        window[3] = static_cast<char>(c);
        if (window[0] == '\r' && window[1] == '\n' && window[2] == '\r' && window[3] == '\n') break;
    }

    // Body: read up to the cap. Stop at close/idle-timeout; a reply over the cap is left truncated (rejected
    // downstream on the over-bound guard, never parsed partially).
    while (bodyLen < bodyCap) {
        const int c = readByte(client, lastMs);
        if (c < 0) break;
        body[bodyLen++] = static_cast<char>(c);
    }
    return status;
}

}  // namespace

UploadResult PwncrackUploader::upload(const char* hc22000Line, const char* key) {
    if (hc22000Line == nullptr || key == nullptr) return UploadResult::Rejected;

    WiFiClientSecure client;
    client.setCACert(kPwncrackRootCaPem);  // the pinned GTS Root R4 (shared with wpa-sec, ADR-0109).
    client.setTimeout(kTlsTimeoutMs / 1000);

    if (!client.connect(kPwncrackHost, kPwncrackPort)) {
        // A TLS/connect failure (including a cert that does not chain to the pinned root, the unconfirmed
        // Cloudflare device leg) is loud and retryable, never a silent fallback that leaks the key/line.
        Serial.println("[ERROR] pwncrack TLS connect failed (cert pin or network)");
        return UploadResult::Rejected;
    }

    // Multipart body: the API key in a `key` text field, the hashline in a `handshake` file field.
    char keyPart[256];
    const int keyPartLen = std::snprintf(
        keyPart, sizeof(keyPart),
        "--%s\r\nContent-Disposition: form-data; name=\"key\"\r\n\r\n%s\r\n", kBoundary, key);
    char filePartHead[192];
    const int filePartHeadLen = std::snprintf(
        filePartHead, sizeof(filePartHead),
        "--%s\r\nContent-Disposition: form-data; name=\"handshake\"; filename=\"capture.hc22000\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        kBoundary);
    char footer[64];
    const int footerLen = std::snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", kBoundary);
    // snprintf returns the length it WOULD have written, so a value >= the buffer size means it truncated
    // (e.g. an over-long key): that would desync Content-Length from the bytes actually sent, so fail closed.
    if (keyPartLen <= 0 || keyPartLen >= static_cast<int>(sizeof(keyPart)) ||
        filePartHeadLen <= 0 || filePartHeadLen >= static_cast<int>(sizeof(filePartHead)) ||
        footerLen <= 0 || footerLen >= static_cast<int>(sizeof(footer))) {
        client.stop();
        return UploadResult::Rejected;
    }
    const size_t lineLen = std::strlen(hc22000Line);
    const size_t contentLength = static_cast<size_t>(keyPartLen) + static_cast<size_t>(filePartHeadLen) +
                                 lineLen + static_cast<size_t>(footerLen);

    client.printf("POST %s HTTP/1.1\r\n", kPwncrackPath);
    client.printf("Host: %s\r\n", kPwncrackHost);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", kBoundary);
    client.printf("Content-Length: %u\r\n", static_cast<unsigned>(contentLength));
    client.printf("Connection: close\r\n\r\n");

    client.write(reinterpret_cast<const uint8_t*>(keyPart), static_cast<size_t>(keyPartLen));
    client.write(reinterpret_cast<const uint8_t*>(filePartHead), static_cast<size_t>(filePartHeadLen));
    client.write(reinterpret_cast<const uint8_t*>(hc22000Line), lineLen);
    client.write(reinterpret_cast<const uint8_t*>(footer), static_cast<size_t>(footerLen));

    char body[kPwncrackResponseMax];
    size_t bodyLen = 0;
    const int status = readResponse(client, body, sizeof(body), bodyLen);
    client.stop();

    const UploadResult result = classifyPwncrackResponse(status, body, bodyLen);
    Serial.printf("[UPLOAD] pwncrack result=%s status=%d bytes=%u\n",
                  result == UploadResult::Accepted ? "accepted" : "rejected", status,
                  static_cast<unsigned>(lineLen));
    return result;
}

}  // namespace sapper

#endif  // UNIT_TEST
