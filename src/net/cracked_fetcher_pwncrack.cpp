/**
 * @file cracked_fetcher_pwncrack.cpp
 * @brief pwncrack download of the account's cracked potfile (ADR-0109 #5). Device-only.
 */
#include "net/cracked_fetcher_pwncrack.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <HTTPClient.h>
#include <Stream.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <cstdio>
#include <cstring>
#include <string_view>

#include "net/pwncrack_root_ca.h"

namespace sapper {
namespace {

constexpr const char* kPwncrackResultsUrlFmt = "https://pwncrack.org/download_potfile_script?key=%s";
constexpr uint32_t kTlsTimeoutSec = 15;
constexpr uint32_t kHttpTimeoutMs = 15000;
constexpr size_t kLineBufferCap = 256;
constexpr size_t kUrlCap = 320;  ///< host+path (~48) + a pwncrack key; fail loud if a key overruns it.

/**
 * A write-only Stream that splits the de-chunked response body into lines and hands each to the sink.
 *
 * This is a verbatim sibling of the wpa-sec fetcher's LineSplittingStream: HTTPClient::writeToStream()
 * de-chunks the body (the HTTPC_TE_CHUNKED branch) while getStreamPtr() would leak chunk-size headers and
 * split a result across a chunk boundary, so we let writeToStream() drive and buffer one line at a time.
 * A line longer than the buffer is abandoned and flagged (overlong): a potfile row is short, so an
 * over-long line is a format anomaly (or a Cloudflare HTML challenge) and fails the sync loud rather than
 * emitting a truncated, mis-parseable record (quality bar §3). Duplicated here at n=2 per the
 * duplicate-don't-abstract rule (quality bar §3); a third stream consumer extracts the shared splitter
 * (LEDGER), as ADR-0080/0082 defer their shared placement helper to the third case.
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
                // drop: the line terminator is just '\n'.
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

FetchResult PwncrackCrackedFetcher::fetch(const char* key, CrackedLineSink& sink) {
    // The pwncrack results endpoint authenticates by a key in the URL query (not a cookie, unlike
    // wpa-sec). Build it with a bound check so an over-long key fails loud rather than issuing a
    // truncated request (quality bar §3). The key is secret, so it is never logged.
    char url[kUrlCap];
    const int urlLen = std::snprintf(url, sizeof(url), kPwncrackResultsUrlFmt, key != nullptr ? key : "");
    if (urlLen <= 0 || urlLen >= static_cast<int>(sizeof(url))) {
        Serial.println("[ERROR] pwncrack download URL buffer overflow");
        return FetchResult::Transport;
    }

    WiFiClientSecure client;
    client.setCACert(kPwncrackRootCaPem);  // GTS Root R4, reused from wpa-sec (ADR-0109 #5; on-air-pending).
    client.setTimeout(kTlsTimeoutSec);

    HTTPClient http;
    http.setReuse(false);
    http.setTimeout(kHttpTimeoutMs);

    if (!http.begin(client, url)) {
        Serial.println("[ERROR] pwncrack download HTTPClient begin failed");
        return FetchResult::Transport;
    }

    const int statusCode = http.GET();
    if (statusCode == 400 || statusCode == 401 || statusCode == 403) {
        http.end();
        return FetchResult::Auth;  // a rejected key: a distinct, actionable cause.
    }
    if (statusCode == 404) {
        http.end();
        return FetchResult::Ok;  // no potfile for this key yet: nothing cracked, a benign empty account.
    }
    if (statusCode != HTTP_CODE_OK) {
        Serial.printf("[ERROR] pwncrack download HTTP %d\n", statusCode);
        http.end();
        return FetchResult::Transport;  // covers negative HTTPClient errors and any other non-200.
    }

    // writeToStream() de-chunks the body and returns a NEGATIVE error on any incomplete transfer, which is
    // what keeps a truncated potfile from being treated as complete (quality bar §3; ADR-0109 #5). The
    // sync's per-source rollback then discards an incomplete pwncrack body without touching the manifest.
    LineSplittingStream out(sink);
    const int written = http.writeToStream(&out);
    out.finish();
    http.end();

    if (written < 0) {
        Serial.printf("[ERROR] pwncrack download body incomplete (%d)\n", written);
        return FetchResult::Transport;
    }
    if (out.overlong()) {
        Serial.println("[ERROR] pwncrack download line too long, treating body as corrupt");
        return FetchResult::Transport;
    }

    Serial.printf("[SYNC] pwncrack download ok bytes=%d lines=%u\n", written,
                  static_cast<unsigned>(out.emitted()));
    return FetchResult::Ok;
}

}  // namespace sapper

#endif  // UNIT_TEST
