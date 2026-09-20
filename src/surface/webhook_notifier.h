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

/// Which engine fact a queued alert speaks for. The webhook now reacts to three fact types, each gated
/// by the policy (ADR-0035); the kind picks the rendered title/body. None carries a password (§4 #14).
enum class WebhookKind : uint8_t {
    Captured,   ///< A fresh handshake was enqueued (essid + bssid; no PSK — a capture has none).
    Cracked,    ///< A genuinely new/changed recovered password (essid + bssid; never the PSK).
    SyncError,  ///< The cracked-sync is failing (no essid/bssid/secret — a static status line).
};

/// What the operator has chosen to push off-device, per notification type (ADR-0035, §4 #19). Sourced
/// from the NVS-backed ProvisioningRecord notify* bools and mapped here; the notifier is handed this
/// policy, not the device record, so its gating stays pure and host-tested. Struct defaults mirror the
/// NVS migration defaults (ADR-0035 decision 5): a default-constructed policy is the safe "cracked only"
/// set, so a device provisioned before this slice keeps its ADR-0023 crack push and gains no surprise
/// capture/sync-error flood. This gates the *transmitting webhook only* — the local LED/screen/serial
/// surfaces render every fact unconditionally (§4 #19).
struct WebhookNotifyPolicy {
    bool onCaptured = false;
    bool onCracked = true;
    bool onSyncError = false;
};

/// One queued alert. ESSID + BSSID only — there is intentionally no password field, so the plaintext
/// PSK cannot reach a third-party push server or notification history (ADR-0023 decision 4). A SyncError
/// alert leaves both empty; a Captured/Cracked alert fills them from the frame-free CaptureFact/
/// CrackedResult (never a pcap byte — §4 #17/#19).
struct WebhookNotification {
    WebhookKind kind = WebhookKind::Cracked;
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
    /// @p policy defaults to the safe "cracked only" set (the struct defaults), so an omitted policy is
    /// exactly the pre-0035 behaviour; production wiring passes the provisioned selector explicitly.
    WebhookNotifier(const char* url, WebhookTransport& transport, WebhookNotifyPolicy policy = {})
        : url_(url), transport_(transport), target_(detectWebhookTarget(url)), policy_(policy) {}

    /// Bus subscription: enqueue the ESSID + BSSID (never a PSK) for each fact the policy enables —
    /// a fresh capture (de-duplicated one-per-BSSID-per-run), a new password, or the onset of a failing
    /// cracked-sync (edge-triggered); ignore every other fact. Enqueue only — no clock, no network
    /// (ADR-0023 decision 1; ADR-0035 decisions 2–3).
    void onAppEvent(const AppEvent& event) override;

    /// Window-sharer: POST every pending notification once; drop the ones that succeed, keep the rest
    /// for the next window (ADR-0023 decision 2). Runs only while the station is up.
    void flushInWindow() override;

    size_t pendingCount() const { return pendingCount_; }
    uint32_t sentCount() const { return sentCount_; }
    /// Notifications dropped because the queue was full when they arrived (fail-loud accounting; §3).
    uint32_t droppedCount() const { return droppedCount_; }
    /// Distinct BSSIDs evicted from the dedup set because it was full (fail-loud accounting; §3). A
    /// non-zero count means the run has seen more networks than kMaxSeenBssids, so an evicted one can
    /// re-push if captured again (ADR-0035 decision 3).
    uint32_t evictedCount() const { return evictedCount_; }

private:
    /// Per-kind rendered strings: the notification title and the trailing phrase, and whether the
    /// essid/bssid target is part of the body (false for SyncError, which names no network).
    struct KindText {
        const char* title;
        const char* phrase;
        bool hasTarget;
    };
    static KindText kindText(WebhookKind kind);

    /// Append one alert to the pending queue; return false (and count it) if the queue is full so the
    /// caller does not record dedup/latch state for an alert that never queued (ADR-0035 decision 2/3).
    bool enqueue(WebhookKind kind, const char* essid, const uint8_t bssid[6]);
    /// Whether @p bssid has already been pushed this run (a scan of the dedup set).
    bool isSeen(const uint8_t bssid[6]) const;
    /// Record @p bssid as pushed this run, evicting the oldest entry (and counting it) when full.
    void recordSeen(const uint8_t bssid[6]);
    /// Pending capacity. A NewPassword arrives only from a sync, and its window flushes it the same
    /// cycle, so the queue rarely holds more than one sync's fresh cracks; a first-sync backlog is
    /// seeded silently and raises no push (ADR-0019 #5). Overflow beyond this is counted, not written.
    static constexpr size_t kMaxPendingNotifications = 8;
    /// ntfy plain body: ESSID (<=32) + " (" + 17-char BSSID + ")\n" + the fixed tail. 160 leaves margin.
    static constexpr size_t kBodyBufferCap = 256;
    /// Capture dedup capacity: distinct BSSIDs pushed once per run before the oldest is evicted. An
    /// architecture-derived bound over a plausible authorized-site AP count (768 bytes at 6 bytes each),
    /// not a measurement — sizing against a real site is a LEDGER item (ADR-0035 decision 3).
    static constexpr size_t kMaxSeenBssids = 128;

    /// Render @p n for the configured target into the scratch buffer and POST it; return the result.
    bool send(const WebhookNotification& n);
    /// Format @p bssid as "AA:BB:CC:DD:EE:FF" into @p out (18 bytes incl. NUL).
    static void formatBssid(const uint8_t bssid[6], char (&out)[18]);

    const char* url_;
    WebhookTransport& transport_;
    WebhookTarget target_;
    WebhookNotifyPolicy policy_;

    WebhookNotification pending_[kMaxPendingNotifications];
    size_t pendingCount_ = 0;
    uint32_t sentCount_ = 0;
    uint32_t droppedCount_ = 0;

    /// One-push-per-BSSID-per-run dedup for captures (ADR-0035 decision 3). A fixed ring: entries append
    /// until full, then the oldest is overwritten. No heap.
    uint8_t seenBssids_[kMaxSeenBssids][6] = {};
    size_t seenCount_ = 0;      ///< Filled slots (0..kMaxSeenBssids).
    size_t seenNext_ = 0;       ///< Next slot to overwrite once full (the oldest — a FIFO ring).
    uint32_t evictedCount_ = 0;
    /// Edge latch for sync-error pushes: true while a failing-sync run is already reported, so a
    /// persistent outage pushes once at onset, not once per failed window (ADR-0035 decision 2).
    bool syncFailing_ = false;

    char bodyBuffer_[kBodyBufferCap];  ///< Render target for one notification during a flush; no heap.
};

}  // namespace sapper
