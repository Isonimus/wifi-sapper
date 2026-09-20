/**
 * @file test_dashboard.cpp
 * @brief Native unit tests for the pure Maintenance-dashboard rendering + backstop (ADR-0039).
 *
 * Proves slice-0040 Scenarios D, E, F off-device: the streamed head/row render the persisted results
 * (PSKs included, and HTML-escaped), each piece fails loud rather than truncating, and the no-activity
 * backstop becomes due at kMaintenanceBackstopMs. The SoftAP serve/stream path is device-only and
 * proven by scripts/0040-maintenance-verify.mjs (Scenario G).
 */
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "net/dashboard.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

static CrackedResult makeResult(const char* essid, const char* psk) {
    CrackedResult r = {};
    std::snprintf(r.essid, sizeof(r.essid), "%s", essid);
    std::snprintf(r.password, sizeof(r.password), "%s", psk);
    return r;
}

// --- Scenario D: the head and rows render the persisted state, PSK included ------------------------

void test_head_renders_the_summary_stats(void) {
    DashboardStats stats = {};
    stats.recoveredCount = 7;
    stats.captureQueueDepth = 3;
    stats.deauthArmed = true;
    stats.everSynced = true;

    char out[kDashboardHeadBufSize];
    const size_t n = buildDashboardHead(stats, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0u, n);
    TEST_ASSERT_NOT_NULL(std::strstr(out, "Recovered networks: 7"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "Captures queued for upload: 3"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "ARMED"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "completed at least once"));
    // The table header opens here; the rows stream after it.
    TEST_ASSERT_NOT_NULL(std::strstr(out, "<th>Password</th>"));
}

void test_head_renders_disarmed_and_never_synced(void) {
    // A device that has never synced and has deauth off: only persisted facts are shown (ADR-0039 #1),
    // and "never synced" comes from the persisted manifest fresh flag, not a RAM last-sync status.
    DashboardStats stats = {};  // everSynced=false, disarmed.
    char out[kDashboardHeadBufSize];
    TEST_ASSERT_GREATER_THAN(0u, buildDashboardHead(stats, out, sizeof(out)));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "disarmed"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "never synced"));
}

void test_row_renders_essid_and_plaintext_psk(void) {
    // ADR-0039 decision 5: the dashboard is the operator's local results viewer, so the row carries the
    // recovered plaintext PSK on purpose. This asserts the PSK reaches the page — a redaction "hardening"
    // would fail here, which is the point.
    const CrackedResult r = makeResult("HomeNet", "correcthorsebattery");
    char out[kDashboardRowBufSize];
    const size_t n = appendCrackedRow(r, out, sizeof(out));
    TEST_ASSERT_GREATER_THAN(0u, n);
    TEST_ASSERT_NOT_NULL(std::strstr(out, "HomeNet"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "correcthorsebattery"));
}

void test_row_html_escapes_a_crafted_essid(void) {
    // An ESSID arrives from the air; a crafted one must render as text, never inject markup.
    const CrackedResult r = makeResult("<script>x</script>", "p@ss&\"word'");
    char out[kDashboardRowBufSize];
    TEST_ASSERT_GREATER_THAN(0u, appendCrackedRow(r, out, sizeof(out)));
    // The raw injection is gone; the escaped entity is present.
    TEST_ASSERT_NULL(std::strstr(out, "<script>"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "&lt;script&gt;"));
    // The PSK's special chars are escaped too (it is shown, but as text).
    TEST_ASSERT_NOT_NULL(std::strstr(out, "&amp;"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "&quot;"));
    TEST_ASSERT_NOT_NULL(std::strstr(out, "&#39;"));
}

// --- slice-0044 Scenario C: the controls are POST-gated for mutation -------------------------------

void test_controls_expose_a_post_resume_and_a_config_link(void) {
    // ADR-0043 decision 3 / §4 #23: resume must be a POST *form* (a GET link would fire on an OS
    // captive-check or a browser prefetch, rebooting the device unbidden), and re-provision is reached
    // at /config. A regression turning resume into a GET link — or dropping either control — fails here.
    TEST_ASSERT_NOT_NULL(std::strstr(kDashboardControls, "method='POST'"));
    TEST_ASSERT_NOT_NULL(std::strstr(kDashboardControls, "action='/resume'"));
    TEST_ASSERT_NOT_NULL(std::strstr(kDashboardControls, "href='/config'"));
    // The results table is closed before the controls render (a <form> must not sit inside the table).
    TEST_ASSERT_NOT_NULL(std::strstr(kDashboardControls, "</table>"));
    // The footer no longer closes the table (the controls chunk now does); it only closes the document.
    TEST_ASSERT_NULL(std::strstr(kDashboardFoot, "</table>"));
    TEST_ASSERT_NOT_NULL(std::strstr(kDashboardFoot, "</body></html>"));
}

// --- Scenario E: fail loud rather than truncate ----------------------------------------------------

void test_head_fails_loud_when_it_does_not_fit(void) {
    DashboardStats stats = {};
    char out[32] = "sentinel";  // far too small for the head.
    TEST_ASSERT_EQUAL_UINT(0u, buildDashboardHead(stats, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);  // emptied, not left as a truncated half-page.
}

void test_row_fails_loud_when_it_does_not_fit(void) {
    const CrackedResult r = makeResult("HomeNet", "correcthorsebattery");
    char out[16] = "sentinel";  // too small for the row markup + fields.
    TEST_ASSERT_EQUAL_UINT(0u, appendCrackedRow(r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

// --- Scenario F: the no-activity backstop becomes due at the bound ---------------------------------

void test_backstop_due_only_at_or_after_the_bound(void) {
    const uint32_t entry = 1000;
    TEST_ASSERT_FALSE(maintenanceBackstopDue(entry, entry));                                // just entered.
    TEST_ASSERT_FALSE(maintenanceBackstopDue(entry, entry + kMaintenanceBackstopMs - 1));   // one ms short.
    TEST_ASSERT_TRUE(maintenanceBackstopDue(entry, entry + kMaintenanceBackstopMs));        // exactly due.
    TEST_ASSERT_TRUE(maintenanceBackstopDue(entry, entry + kMaintenanceBackstopMs + 5000)); // past due.
}

void test_backstop_activity_pushes_the_due_time_forward(void) {
    // A request at t=T resets the activity clock, so the device that was about to resume stays up.
    const uint32_t wasAboutToResume = kMaintenanceBackstopMs - 1;
    TEST_ASSERT_FALSE(maintenanceBackstopDue(0, wasAboutToResume));
    // Activity recorded at `wasAboutToResume`: now not due again until another full window elapses.
    TEST_ASSERT_FALSE(maintenanceBackstopDue(wasAboutToResume, wasAboutToResume + kMaintenanceBackstopMs - 1));
    TEST_ASSERT_TRUE(maintenanceBackstopDue(wasAboutToResume, wasAboutToResume + kMaintenanceBackstopMs));
}

void test_backstop_is_wrap_safe_across_millis_rollover(void) {
    // lastActivity just before the 32-bit wrap, now just after: the unsigned difference is the true
    // small elapsed interval, so the backstop is NOT spuriously due right after a rollover.
    const uint32_t nearWrap = 0xFFFFFFFFu - 100u;
    TEST_ASSERT_FALSE(maintenanceBackstopDue(nearWrap, nearWrap + 200u));  // 200 ms elapsed, wrapped.
    TEST_ASSERT_TRUE(maintenanceBackstopDue(nearWrap, nearWrap + kMaintenanceBackstopMs));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_head_renders_the_summary_stats);
    RUN_TEST(test_head_renders_disarmed_and_never_synced);
    RUN_TEST(test_row_renders_essid_and_plaintext_psk);
    RUN_TEST(test_row_html_escapes_a_crafted_essid);
    RUN_TEST(test_controls_expose_a_post_resume_and_a_config_link);
    RUN_TEST(test_head_fails_loud_when_it_does_not_fit);
    RUN_TEST(test_row_fails_loud_when_it_does_not_fit);
    RUN_TEST(test_backstop_due_only_at_or_after_the_bound);
    RUN_TEST(test_backstop_activity_pushes_the_due_time_forward);
    RUN_TEST(test_backstop_is_wrap_safe_across_millis_rollover);
    return UNITY_END();
}
