/**
 * @file hc22000_uploader.h
 * @brief The pwncrack upload seam: hand one hc22000 line to pwncrack and classify the answer (ADR-0109).
 *
 * The sibling of the pcap `Uploader` (net/uploader.h), not a shared polymorphic backend interface
 * (ADR-0109 decision #1: duplicate-don't-abstract at the second backend). Where wpa-sec takes a raw pcap,
 * pwncrack takes a hashcat `.hc22000` line (WPA*02 4-way from ADR-0108 buildHc22000Line, or WPA*01 PMKID from
 * ADR-0110). The device backs this with
 * uploader_pwncrack.h (a WiFiClientSecure multipart POST to pwncrack.org); a host test backs it with a fake
 * returning a scripted result. The seam is deliberately narrow (one hashline in, one classification out) so
 * the dual-backend drain above it is host-tested without a network (ADR-0004 lane 1).
 *
 * The result reuses the shared UploadResult enum (ADR-0109 decision #1). pwncrack has **no duplicate
 * signal** (a fresh submit and a re-submit both answer HTTP 200 `{"success":true}`, measured 2026-11-05,
 * ADR-0109 amendment), so an implementation never returns Duplicate: it is Accepted or Rejected. Anything
 * that is not a positive Accepted (non-200, non-JSON body, missing/false success, transport/TLS failure) is
 * Rejected, which keeps the capture and retries it: an unrecognised response is never assumed success
 * (quality bar §3). This matters less than for wpa-sec because pwncrack is the secondary backend under
 * both-terminal removal (§4 #46b), so a kept capture is never a lost handshake.
 */
#pragma once

#include "net/uploader.h"  // reuse UploadResult (ADR-0109 decision #1: one shared vocabulary).

namespace sapper {

/// Uploads one NUL-terminated hc22000 line to pwncrack. Device-backed over TLS; host-backed by a fake.
class Hc22000Uploader {
public:
    virtual ~Hc22000Uploader() = default;

    /// POST @p hc22000Line authenticated by the pwncrack @p key, and classify the response. Must return
    /// Rejected (never assume success) on any transport failure or unrecognised body; never Duplicate.
    virtual UploadResult upload(const char* hc22000Line, const char* key) = 0;
};

}  // namespace sapper
