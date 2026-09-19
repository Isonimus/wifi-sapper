/**
 * @file cracked_fetcher.h
 * @brief The download seam: read the account's cracked set from wpa-sec line by line (ADR-0019
 *        decision #1).
 *
 * The pure CrackedSync talks only to this abstract fetcher. The device backs it with
 * cracked_fetcher_wpasec.h — a `GET /?api&dl=1` with the key cookie over a WiFiClientSecure pinned to
 * the same GTS Root R4 as the upload (ADR-0017 decision #1), read through HTTPClient so the stack
 * de-frames any `Transfer-Encoding: chunked` body (a split line here corrupts a result, not just a
 * retry — decision #1). A host test backs it with a fake replaying a captured body's lines, or a
 * scripted failure.
 *
 * The body is delivered one line at a time to a CrackedLineSink rather than returned as one buffer:
 * the account's whole cracked set can be large, and streaming lets the device parse without holding
 * the raw HTTP body (headers, client BSSIDs, chunk framing) in RAM. Whether the transfer as a whole
 * succeeded is the fetcher's return value, so a partial read is never mistaken for a complete account
 * (quality bar §3): the sync applies nothing unless the fetch returned Ok.
 */
#pragma once

#include <string_view>

namespace sapper {

/// How the download as a whole went. Only Ok means the body was fully received; the sync mutates the
/// manifest only on Ok (ADR-0019 decision #7). Transport covers connect/TLS/timeout/short-read
/// failures; Auth is a rejected key (a distinct, actionable cause worth reporting separately).
enum class FetchResult { Ok, Transport, Auth };

/// Receives the download body one line at a time, terminators tolerated by the parser. Implemented by
/// CrackedSync; the fetcher calls onLine() for each line it reads off the stream.
class CrackedLineSink {
public:
    virtual ~CrackedLineSink() = default;
    virtual void onLine(std::string_view line) = 0;
};

/// Fetches the account's cracked set. Device-backed over pinned TLS; host-backed by a replaying fake.
class CrackedResultsFetcher {
public:
    virtual ~CrackedResultsFetcher() = default;

    /// Issue the download authenticated by @p key, delivering each body line to @p sink, and return
    /// whether the whole transfer succeeded. Must return non-Ok (never Ok on a partial body) on any
    /// transport/TLS failure so the sync does not treat a truncated account as complete.
    virtual FetchResult fetch(const char* key, CrackedLineSink& sink) = 0;
};

}  // namespace sapper
