/**
 * @file uploader_wpasec.h
 * @brief wpa-sec TLS upload backing of the Uploader seam (ADR-0017 decisions #1, #2). Device-only.
 *
 * Opens a WiFiClientSecure validated against the single pinned GTS Root R4 (wpasec_root_ca.h), POSTs
 * one in-RAM pcap to wpa-sec.stanev.org as multipart/form-data with the operator's key in the `key`
 * cookie, and classifies the text response fail-closed: Accepted / Duplicate only on a positive
 * wpa-sec token, everything else (non-2xx, unrecognised body, transport/TLS failure) Rejected so the
 * capture is kept and retried, never assumed uploaded (quality bar §3). The endpoint, cookie, field
 * name, and success/duplicate tokens are the contract the on-air verify (slice-0018 Scenario G) pins
 * against the live service; they match the proven Adversary implementation.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/uploader.h"

namespace sapper {

class WpaSecUploader : public Uploader {
public:
    UploadResult upload(const uint8_t* pcap, size_t len, const char* key) override;
};

}  // namespace sapper

#endif  // UNIT_TEST
