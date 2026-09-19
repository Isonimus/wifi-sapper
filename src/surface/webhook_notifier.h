/**
 * @file webhook_notifier.h
 * @brief The push-notification surface: observe a crack on the bus, POST it in an STA window (ADR-0023).
 *
 * The second slice-7 surface (ADR-0021) and the first that *transmits*. It subscribes to the event bus
 * as an EventSink and, on a NewPassword fact, copies a compact WebhookNotification — the ESSID and
 * BSSID, and deliberately NOT the plaintext PSK (ADR-0023 decision 4; the type has no password field,
 * so no rendering path can leak it) — into a fixed pending queue. It transmits nothing there: a POST
 * needs connectivity, which exists only inside a supervisor-owned STA window (ADR-0023). So it also
 * implements WindowNotifier: the supervisor calls flushInWindow() while the station is up (after the
 * drain and the cracked-results sync, before tear-down), and only then does it POST each pending
 * notification through the WebhookTransport seam, keeping any that fail for the next window.
 *
 * All policy — target detection, per-target message rendering (including JSON-escaping an ESSID that
 * may contain quotes), the pending queue and its overflow accounting, and the send-and-keep flush — is
 * pure and host-tested against a fake transport (ADR-0004 lane 1). Only the HTTPS POST is device code
 * (webhook_transport_esp32.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/event_bus.h"
#include "net/cracked_result.h"     // kCrackedEssidCap — the ESSID capacity the notification mirrors.
#include "net/window_notifier.h"

namespace sapper {

/// The wire format a target speaks. Detected once from the configured URL's host (ADR-0023 decision 4).
enum class WebhookTarget : uint8_t {
    Ntfy,     ///< ntfy.sh or a self-hosted ntfy: a plain-text body with Title/Priority headers.
    Discord,  ///< a Discord webhook: a JSON body {"content":"…"} with Content-Type: application/json.
};

/// Pick the wire format from @p url's host: discord.com / discordapp.com → Discord, else ntfy. Pure so
/// it is host-tested directly (Scenario H). A null/empty URL is moot (the surface is disabled then).
WebhookTarget detectWebhookTarget(const char* url);

/// One queued alert. ESSID + BSSID only — there is intentionally no password field, so the plaintext
/// PSK cannot reach a third-party push server or notification history (ADR-0023 decision 4).
struct WebhookNotification {
    char essid[kCrackedEssidCap] = {0};
    uint8_t bssid[6] = {0};
};

/// A rendered HTTP request the transport POSTs verbatim. All fields borrowed from the notifier's scratch
/// buffers for the duration of the post() call only (like AppEvent's borrowed payloads, ADR-0021).
struct WebhookRequest {
    const char* url = nullptr;          ///< The configured endpoint to POST to.
    const char* contentType = nullptr;  ///< Content-Type header, or null for ntfy's plain body.
    const char* title = nullptr;        ///< ntfy Title header, or null for Discord.
    const char* body = nullptr;         ///< The POST body (plain text for ntfy, JSON for Discord).
};

/// The push HTTPS seam. Device-backed by Esp32WebhookTransport (WiFiClientSecure + system CA bundle);
/// host-backed by a scripted fake. Narrow — one request in, success/failure out — so all the notifier
/// policy above it is host-tested without a network (ADR-0004 lane 1).
class WebhookTransport {
public:
    virtual ~WebhookTransport() = default;

    /// POST @p request now (called only inside an STA window). Return true only on a 2xx; any transport
    /// failure or non-2xx is false, so the notifier keeps the alert and retries next window.
    virtual bool post(const WebhookRequest& request) = 0;
};

/**
 * @brief The push-notification surface. Subscribe it to the bus and set it on the supervisor as the
 *        WindowNotifier; it enqueues on NewPassword and POSTs on flushInWindow().
 *
 * Holds the URL by borrowed pointer (it lives in the owned credentials for the device's whole run) and
 * detects the target once at construction. No heap: a fixed pending queue and fixed render scratch.
 */
class WebhookNotifier : public EventSink, public WindowNotifier {
public:
    WebhookNotifier(const char* url, WebhookTransport& transport)
        : url_(url), transport_(transport), target_(detectWebhookTarget(url)) {}

    /// Bus subscription: on NewPassword, copy the ESSID + BSSID into the pending queue (never the PSK);
    /// ignore every other fact. Enqueue only — no clock, no network (ADR-0023 decision 1).
    void onAppEvent(const AppEvent& event) override;

    /// Window-sharer: POST every pending notification once; drop the ones that succeed, keep the rest
    /// for the next window (ADR-0023 decision 2). Runs only while the station is up.
    void flushInWindow() override;

    size_t pendingCount() const { return pendingCount_; }
    uint32_t sentCount() const { return sentCount_; }
    /// Notifications dropped because the queue was full when they arrived (fail-loud accounting; §3).
    uint32_t droppedCount() const { return droppedCount_; }

private:
    /// Pending capacity. A NewPassword arrives only from a sync, and its window flushes it the same
    /// cycle, so the queue rarely holds more than one sync's fresh cracks; a first-sync backlog is
    /// seeded silently and raises no push (ADR-0019 #5). Overflow beyond this is counted, not written.
    static constexpr size_t kMaxPendingNotifications = 8;
    /// ntfy plain body: ESSID (<=32) + " (" + 17-char BSSID + ")\n" + the fixed tail. 160 leaves margin.
    static constexpr size_t kBodyBufferCap = 256;

    /// Render @p n for the configured target into the scratch buffer and POST it; return the result.
    bool send(const WebhookNotification& n);
    /// Format @p bssid as "AA:BB:CC:DD:EE:FF" into @p out (18 bytes incl. NUL).
    static void formatBssid(const uint8_t bssid[6], char (&out)[18]);

    const char* url_;
    WebhookTransport& transport_;
    WebhookTarget target_;

    WebhookNotification pending_[kMaxPendingNotifications];
    size_t pendingCount_ = 0;
    uint32_t sentCount_ = 0;
    uint32_t droppedCount_ = 0;

    char bodyBuffer_[kBodyBufferCap];  ///< Render target for one notification during a flush; no heap.
};

}  // namespace sapper
