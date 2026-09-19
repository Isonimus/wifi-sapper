/**
 * @file uploader.h
 * @brief The TLS-upload seam: hand one pcap to wpa-sec and classify the answer (ADR-0017 decision #2).
 *
 * The pure drain supervisor talks only to this abstract Uploader. The device backs it with
 * uploader_wpasec.h (WiFiClientSecure against the pinned GTS Root R4, a multipart POST with the key
 * cookie); a host test backs it with a fake returning a scripted sequence of results. The seam is
 * deliberately narrow — one pcap in, one classification out — so all the arbitration/backoff logic
 * above it is host-tested without a network (ADR-0004 lane 1).
 *
 * The result is a strict, fail-loud classification (quality bar §3, ADR-0017 decision #2): a capture
 * is deleted from the queue only on a *positive* Accepted or Duplicate. Anything else — a non-2xx
 * status, a body matching no known success token, a transport/TLS failure, a truncated read — is
 * Rejected, which keeps the capture and retries it. An unrecognised response is therefore never
 * assumed to be success, so a change of wording at wpa-sec fails loud rather than silently dropping
 * captures.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// How wpa-sec answered one upload. Accepted and Duplicate are both terminal successes — the capture
/// is off our hands (Duplicate means wpa-sec already holds it). Rejected covers every non-success,
/// including transport errors, and means keep-and-retry.
enum class UploadResult { Accepted, Duplicate, Rejected };

/// Uploads one serialized pcap to wpa-sec. Device-backed over TLS; host-backed by a scripted fake.
class Uploader {
public:
    virtual ~Uploader() = default;

    /// POST @p len bytes of pcap authenticated by the wpa-sec @p key, and classify the response.
    /// Must return Rejected (never assume success) on any transport failure or unrecognised body.
    virtual UploadResult upload(const uint8_t* pcap, size_t len, const char* key) = 0;
};

}  // namespace sapper
