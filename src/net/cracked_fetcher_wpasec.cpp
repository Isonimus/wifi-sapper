/**
 * @file cracked_fetcher_wpasec.cpp
 * @brief wpa-sec download of the account's cracked results (ADR-0019 decision #1). Device-only.
 */
#include "net/cracked_fetcher_wpasec.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstring>
#include <string_view>

#include "net/wpasec_root_ca.h"

namespace sapper {
namespace {

constexpr const char* kWpaSecUrl = "https://wpa-sec.stanev.org/?api&dl=1";
constexpr uint32_t kTlsTimeoutSec = 15;
constexpr uint32_t kHttpTimeoutMs = 15000;
constexpr size_t kLineBufferCap = 256;

/**
 * A write-only Stream that splits the de-chunked response body into lines and hands each to the sink.
 *
 * HTTPClient::writeToStream() is what actually de-chunks the body (the HTTPC_TE_CHUNKED branch);
 * getStreamPtr() returns the RAW client, which on a chunked response would leak chunk-size headers into
 * the data and split a result across a chunk boundary — exactly the corruption ADR-0019 decision #1 must
 * avoid. So we let writeToStream() drive the transfer and feed its de-chunked bytes here, buffering only
 * one line at a time (never the whole account). A line longer than the buffer is abandoned and flagged
 * (overlong): a well-formed wpa-sec line is ~120 bytes, so an over-long line is a format anomaly and
 * fails the whole sync loud rather than emitting a truncated, mis-parseable record (quality bar §3).
 */
class LineSplittingStream : public Stream {
public:
    explicit LineSplittingStream(CrackedLineSink& sink) : sink_(sink) {}

    size_t write(uint8_t b) override { return write(&b, 1); }

    size_t write(const uint8_t* buf, size_t size) override {
        for (size_t i = 0; i < size; ++i) {
            const char c = static_cast<char>(buf[i]);
            if (c == '\n') {
                emitLine();
            } else if (c == '\r') {
                // drop — the line terminator is just '\n'.
            } else if (skipping_) {
                // this line already overflowed the buffer: swallow the rest until the newline.
            } else if (lineLen_ < kLineBufferCap - 1) {
                line_[lineLen_++] = c;
            } else {
                overlong_ = true;   // fail the sync; do not emit a truncated record.
                skipping_ = true;
                lineLen_ = 0;
            }
        }
        return size;  // always consume everything; completeness is writeToStream()'s return value.
    }

    /// Emit any trailing partial line the body ended without a final newline.
    void finish() { emitLine(); }

    bool overlong() const { return overlong_; }
    size_t emitted() const { return emitted_; }

    // Stream is abstract; the read side is never used by writeToStream().
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }

private:
    void emitLine() {
        if (!skipping_ && lineLen_ > 0) {
            sink_.onLine(std::string_view(line_, lineLen_));
            ++emitted_;
        }
        lineLen_ = 0;
        skipping_ = false;
    }

    CrackedLineSink& sink_;
    char line_[kLineBufferCap];
    size_t lineLen_ = 0;
    size_t emitted_ = 0;
    bool skipping_ = false;
    bool overlong_ = false;
};

}  // namespace

FetchResult WpaSecCrackedFetcher::fetch(const char* key, CrackedLineSink& sink) {
    WiFiClientSecure client;
    client.setCACert(kWpaSecRootCaPem);  // the pinned trust anchor (ADR-0017 decision #1).
    client.setTimeout(kTlsTimeoutSec);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, kWpaSecUrl)) {
        Serial.println("[ERROR] wpa-sec download HTTPClient begin failed");
        return FetchResult::Transport;
    }

    // Authenticate with the operator's key in the cookie, exactly as the upload does.
    char cookieBuf[128];
    const int cookieLen = std::snprintf(cookieBuf, sizeof(cookieBuf), "key=%s", key);
    if (cookieLen <= 0 || cookieLen >= static_cast<int>(sizeof(cookieBuf))) {
        Serial.println("[ERROR] wpa-sec download cookie buffer overflow");
        http.end();
        return FetchResult::Transport;
    }
    http.addHeader("Cookie", cookieBuf);

    const int statusCode = http.GET();
    if (statusCode == 401 || statusCode == 403) {
        http.end();
        return FetchResult::Auth;  // a rejected key — a distinct, actionable cause.
    }
    if (statusCode != HTTP_CODE_OK) {
        Serial.printf("[ERROR] wpa-sec download HTTP %d\n", statusCode);
        http.end();
        return FetchResult::Transport;  // covers negative HTTPClient errors and any non-200.
    }

    // writeToStream() de-chunks the body and returns the byte count, or a NEGATIVE error on any
    // incomplete transfer (connection lost, read timeout, or bytes != Content-Length / chunk total). That
    // negative is what stops a truncated account from ever being treated as complete (ADR-0019 decision
    // #7; slice-0020 Scenario I): we return Ok only on a non-negative result with no over-long line.
    LineSplittingStream out(sink);
    const int written = http.writeToStream(&out);
    out.finish();
    http.end();

    if (written < 0) {
        Serial.printf("[ERROR] wpa-sec download body incomplete (%d)\n", written);
        return FetchResult::Transport;
    }
    if (out.overlong()) {
        Serial.println("[ERROR] wpa-sec download line too long — treating body as corrupt");
        return FetchResult::Transport;
    }

    Serial.printf("[SYNC] download ok bytes=%d lines=%u\n", written,
                  static_cast<unsigned>(out.emitted()));
    return FetchResult::Ok;
}

}  // namespace sapper

#endif  // UNIT_TEST
