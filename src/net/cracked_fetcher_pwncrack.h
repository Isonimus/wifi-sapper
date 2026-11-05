/**
 * @file cracked_fetcher_pwncrack.h
 * @brief pwncrack download of the account's cracked potfile (ADR-0109 #5). Device-only.
 *
 * Implements the SAME CrackedResultsFetcher seam the wpa-sec fetcher does (ADR-0109 reuses the seam and
 * FetchResult rather than adding a polymorphic backend interface): a `GET
 * https://pwncrack.org/download_potfile_script?key=<key>` over TLS, streaming the colon-potfile body one
 * line at a time to the sink. CrackedSync selects the pwncrack potfile parser for this source's lines
 * (`setPwncrackSource`); the fetcher itself is parser-agnostic, exactly like the wpa-sec one.
 *
 * The TLS trust anchor is reused from wpa-sec (GTS Root R4, via pwncrack_root_ca.h) because the
 * 2026-11-05 workstation probe saw a GTS chain fronted by Cloudflare; a device/region edge cert mismatch
 * fails the handshake loud (Transport + [ERROR] log), never a silent unverified connect. Two device-leg
 * unknowns remain on-air-pending (LEDGER), both handled fail-closed here: (i) the Cloudflare edge may
 * answer the device (a different TLS client) with a bot-challenge interstitial rather than the potfile,
 * which the streaming parser treats as all-malformed and the sync drops without mutating the manifest;
 * (ii) a `Transfer-Encoding: chunked` body is de-framed by HTTPClient::writeToStream exactly as the
 * wpa-sec path relies on, so a chunk boundary never splits a result. Device-only and unverified until
 * the operator's on-air fetch run, like uploader_pwncrack.cpp.
 *
 * Completeness is writeToStream's return value (NEGATIVE on any short read), so a truncated potfile is
 * never reported Ok and the sync's per-source rollback discards it (quality bar §3; ADR-0109 #5). A
 * rejected key returns Auth; a 404 (no potfile yet, nothing cracked) is a benign empty Ok; any other
 * connection/timeout/short-read/non-200 is Transport.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/cracked_fetcher.h"

namespace sapper {

class PwncrackCrackedFetcher : public CrackedResultsFetcher {
public:
    FetchResult fetch(const char* key, CrackedLineSink& sink) override;
};

}  // namespace sapper

#endif  // UNIT_TEST
