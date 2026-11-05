/**
 * @file uploader_pwncrack.h
 * @brief pwncrack TLS upload backing of the Hc22000Uploader seam (ADR-0109). Device-only.
 *
 * The sibling of WpaSecUploader (uploader_wpasec.h): opens a WiFiClientSecure validated against the pinned
 * GTS Root R4 (pwncrack_root_ca.h), POSTs one hc22000 line to pwncrack.org/upload_handshake as
 * multipart/form-data (a `key` text field + a `handshake` file field), and classifies the JSON response
 * fail-closed through the pure classifyPwncrackResponse (net/pwncrack_response.h): Accepted only on HTTP 200
 * with `{"success": true}`, everything else Rejected so the capture is kept and retried, never assumed
 * uploaded (quality bar §3). The endpoint, field names, and response contract are the ones the firsthand API
 * re-verification (ADR-0109 context) and the upload-response probe (ADR-0109 amendment) pinned; the device
 * leg behind Cloudflare is confirmed on the operator on-air run (LEDGER).
 */
#pragma once

#ifndef UNIT_TEST

#include "net/hc22000_uploader.h"

namespace sapper {

class PwncrackUploader : public Hc22000Uploader {
public:
    UploadResult upload(const char* hc22000Line, const char* key) override;
};

}  // namespace sapper

#endif  // UNIT_TEST
