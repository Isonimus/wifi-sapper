/**
 * @file webhook_transport_esp32.h
 * @brief The device push transport: an HTTPS POST to the configured webhook (ADR-0023). Device-only.
 *
 * Implements the WebhookTransport seam over WiFiClientSecure + HTTPClient. Unlike the wpa-sec upload,
 * which pins one self-signed root for its FIXED host (ADR-0017 decision #1), the webhook targets an
 * arbitrary, operator-configured, often CDN-fronted host (ntfy.sh, a self-hosted ntfy, discord.com),
 * so it verifies the peer against the ESP-IDF default full Mozilla root-CA bundle already linked into
 * the framework (setCACertBundle) — the right trust model for a rotating public endpoint, at zero
 * incremental flash (the bundle is in libmbedtls.a regardless). A cert that chains to no trusted root
 * fails the handshake loud (a logged error, never an unverified fallback — quality bar §3, ADR-0023).
 */
#pragma once

#ifndef UNIT_TEST

#include "surface/webhook_notifier.h"

namespace sapper {

class Esp32WebhookTransport : public WebhookTransport {
public:
    bool post(const WebhookRequest& request) override;
};

}  // namespace sapper

#endif  // UNIT_TEST
