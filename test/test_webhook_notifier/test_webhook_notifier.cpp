/**
 * @file test_webhook_notifier.cpp
 * @brief Native unit tests for the push-notification surface policy (ADR-0023).
 *
 * The notifier is pure, so all of it is asserted here against a fake transport: it enqueues only on a
 * NewPassword fact, ignores every other event, POSTs each pending notification once and keeps the ones
 * that fail for the next window, accounts for overflow, and renders the right wire format per target —
 * an ntfy plain body with a Title header, or a JSON Discord body with the ESSID escaped. Two tests pin
 * the security invariant the type enforces: the plaintext PSK appears in no rendered output. The HTTPS
 * POST itself is the on-air verify's job (Scenario K, lane 3); *what* is sent is proven here.
 */
#include <unity.h>

#include <cstring>
#include <string>

#include "net/cracked_result.h"
#include "net/cracked_sync.h"        // SyncOutcome — a payload for the "ignored facts" test.
#include "net/upload_supervisor.h"   // DrainOutcome — a payload for the "ignored facts" test.
#include "support/fake_webhook_transport.h"
#include "surface/webhook_notifier.h"

using namespace sapper;
using sapper_test::FakeWebhookTransport;

void setUp(void) {}
void tearDown(void) {}

static const uint8_t kBssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/// Publish a NewPassword through the surface's bus seam, carrying an ESSID, a BSSID, and a PSK — the
/// PSK is present precisely so the tests can assert it never reaches the rendered output.
static void feed(WebhookNotifier& n, const char* essid, const char* password) {
    CrackedResult r;
    std::snprintf(r.essid, sizeof(r.essid), "%s", essid);
    std::memcpy(r.bssid, kBssid, sizeof(r.bssid));
    std::snprintf(r.password, sizeof(r.password), "%s", password);
    n.onAppEvent(AppEvent::newPassword(r));
}

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

void test_new_password_is_enqueued(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/topic", t);

    feed(n, "HomeNet", "secret-psk");

    TEST_ASSERT_EQUAL_size_t(1, n.pendingCount());
    TEST_ASSERT_EQUAL_size_t(0, t.posts.size());  // enqueue only — no transmit outside a window.
}

void test_non_crack_facts_are_ignored(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/topic", t);
    DrainOutcome drain;
    SyncOutcome sync;

    n.onAppEvent(AppEvent::drainStarted());
    n.onAppEvent(AppEvent::drainCompleted(drain));
    n.onAppEvent(AppEvent::syncCompleted(sync));
    n.onAppEvent(AppEvent::firstSyncSummary(42));  // a seeded backlog raises no push (ADR-0019 #5).

    TEST_ASSERT_EQUAL_size_t(0, n.pendingCount());
}

void test_flush_sends_all_pending_and_clears(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/topic", t);
    feed(n, "NetA", "pa");
    feed(n, "NetB", "pb");

    n.flushInWindow();

    TEST_ASSERT_EQUAL_size_t(2, t.posts.size());
    TEST_ASSERT_EQUAL_size_t(0, n.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(2, n.sentCount());
}

void test_failed_send_is_kept_and_retried_next_window(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/topic", t);
    feed(n, "NetA", "pa");

    t.succeed = false;
    n.flushInWindow();  // the endpoint is down: the notification must be kept, not lost.
    TEST_ASSERT_EQUAL_size_t(1, n.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(0, n.sentCount());

    t.succeed = true;
    n.flushInWindow();  // the next window succeeds.
    TEST_ASSERT_EQUAL_size_t(0, n.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(1, n.sentCount());
}

void test_overflow_is_counted_not_written(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/topic", t);

    for (int i = 0; i < 10; ++i) feed(n, "Net", "pw");  // capacity is 8.

    TEST_ASSERT_EQUAL_size_t(8, n.pendingCount());
    TEST_ASSERT_EQUAL_UINT32(2, n.droppedCount());
}

void test_ntfy_rendering(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://ntfy.sh/my-sapper", t);
    feed(n, "HomeNet", "super-secret-psk");

    n.flushInWindow();

    TEST_ASSERT_EQUAL_size_t(1, t.posts.size());
    const auto& p = t.posts[0];
    TEST_ASSERT_TRUE(p.hadTitle);          // ntfy carries the title in a header,
    TEST_ASSERT_FALSE(p.hadContentType);   // and a plain-text body (no JSON content type).
    TEST_ASSERT_TRUE(contains(p.body, "HomeNet"));
    TEST_ASSERT_TRUE(contains(p.body, "AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_FALSE(contains(p.body, "super-secret-psk"));  // the PSK never leaves the appliance.
}

void test_discord_rendering_escapes_and_omits_psk(void) {
    FakeWebhookTransport t;
    WebhookNotifier n("https://discord.com/api/webhooks/1/abc", t);
    feed(n, "Net\"X", "super-secret-psk");  // an ESSID with a quote must not break the JSON.

    n.flushInWindow();

    TEST_ASSERT_EQUAL_size_t(1, t.posts.size());
    const auto& p = t.posts[0];
    TEST_ASSERT_TRUE(p.hadContentType);
    TEST_ASSERT_EQUAL_STRING("application/json", p.contentType.c_str());
    TEST_ASSERT_FALSE(p.hadTitle);
    TEST_ASSERT_TRUE(contains(p.body, "\"content\""));      // a JSON object,
    TEST_ASSERT_TRUE(contains(p.body, "Net\\\"X"));         // with the quote escaped as \" ,
    TEST_ASSERT_TRUE(contains(p.body, "AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_FALSE(contains(p.body, "super-secret-psk"));  // the PSK never leaves the appliance.
}

void test_detect_target_from_url(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Discord),
                          static_cast<int>(detectWebhookTarget("https://discord.com/api/webhooks/1/a")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Discord),
                          static_cast<int>(detectWebhookTarget("https://discordapp.com/api/webhooks/1/a")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Discord),  // a Discord subdomain is Discord,
                          static_cast<int>(detectWebhookTarget("https://canary.discord.com/api/webhooks/1/a")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Ntfy),
                          static_cast<int>(detectWebhookTarget("https://ntfy.sh/my-topic")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Ntfy),
                          static_cast<int>(detectWebhookTarget("https://push.example.com/hook")));
    // Host-scoped, not a whole-URL substring: "discord.com" in a ntfy topic PATH is still ntfy, and a
    // mere suffix ("notdiscord.com") is not Discord.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Ntfy),
                          static_cast<int>(detectWebhookTarget("https://ntfy.example.com/discord.com-alerts")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(WebhookTarget::Ntfy),
                          static_cast<int>(detectWebhookTarget("https://notdiscord.com/api/webhooks/1/a")));
}

void test_invalid_utf8_essid_is_sanitized_before_send(void) {
    // A non-UTF-8 SSID byte must never reach the body: Discord rejects a non-UTF-8 JSON body (400), and
    // a kept-for-retry failure would wedge the queue and eventually drop new cracks. It is replaced with
    // '?', while a legitimately international (valid UTF-8) SSID survives intact.
    FakeWebhookTransport t;
    WebhookNotifier n("https://discord.com/api/webhooks/1/abc", t);
    feed(n, "A\xFF" "B", "psk");    // 0xFF is never a valid UTF-8 byte,
    feed(n, "caf\xC3\xA9", "psk");  // "café" — a valid 2-byte UTF-8 sequence.

    n.flushInWindow();

    TEST_ASSERT_EQUAL_size_t(2, t.posts.size());
    TEST_ASSERT_TRUE(contains(t.posts[0].body, "A?B"));                          // invalid byte sanitized,
    TEST_ASSERT_EQUAL_size_t(std::string::npos, t.posts[0].body.find('\xFF'));    // and gone from the body,
    TEST_ASSERT_TRUE(contains(t.posts[1].body, "caf\xC3\xA9"));                  // valid UTF-8 preserved.
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_new_password_is_enqueued);
    RUN_TEST(test_non_crack_facts_are_ignored);
    RUN_TEST(test_flush_sends_all_pending_and_clears);
    RUN_TEST(test_failed_send_is_kept_and_retried_next_window);
    RUN_TEST(test_overflow_is_counted_not_written);
    RUN_TEST(test_ntfy_rendering);
    RUN_TEST(test_discord_rendering_escapes_and_omits_psk);
    RUN_TEST(test_detect_target_from_url);
    RUN_TEST(test_invalid_utf8_essid_is_sanitized_before_send);
    return UNITY_END();
}
