/**
 * @file cracked_fetcher_wpasec.h
 * @brief wpa-sec download of the account's cracked results (ADR-0019 decision #1).
 *        Device-only.
 *
 * Implements the CrackedResultsFetcher seam as a TLS-pinned HTTPS download from wpa-sec.
 * A `GET /?api&dl=1` authenticated by the operator's key cookie returns the cracked results
 * one line per result (ADR-0019 decision #1).
 *
 * The body is read through `HTTPClient::writeToStream()`, NOT `getStreamPtr()`: only writeToStream
 * de-frames a `Transfer-Encoding: chunked` body (getStreamPtr hands back the raw client, whose bytes
 * still carry chunk-size headers and can split a result across a chunk boundary — the exact corruption
 * decision #1 exists to prevent). writeToStream feeds the de-chunked bytes into a small line-splitting
 * Stream (the .cpp) that emits whole lines to the sink, buffering one line at a time rather than the whole
 * account. Crucially, writeToStream also returns a NEGATIVE error on any incomplete transfer (bytes !=
 * Content-Length / chunk total, connection lost, read timeout), so a truncated account can never be
 * reported Ok. The TLS connection pins the same GTS Root R4 as the upload (ADR-0017 decision #1), so a
 * transport failure is loud and never silently retried with an unverified peer.
 *
 * A rejected key returns Auth; a connection/timeout/short-read/incomplete body is Transport; only a
 * fully-received body returns Ok. The sync applies changes only on Ok, so a partial read never corrupts
 * the account (quality bar §3, ADR-0019 decision #7).
 */
#pragma once

#ifndef UNIT_TEST

#include "net/cracked_fetcher.h"

namespace sapper {

class WpaSecCrackedFetcher : public CrackedResultsFetcher {
public:
    FetchResult fetch(const char* key, CrackedLineSink& sink) override;
};

}  // namespace sapper

#endif  // UNIT_TEST
