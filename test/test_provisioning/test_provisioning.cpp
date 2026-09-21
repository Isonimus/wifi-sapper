/**
 * @file test_provisioning.cpp
 * @brief Native unit tests for the pure provisioning logic (ADR-0006, slice-0007 Scenario A).
 *
 * Proves the two off-device decisions every later network action rests on — is a credential
 * triad usable, and does boot open the portal or go straight to STA — plus the SoftAP naming a
 * screenless operator matches against the README. The NVS store, portal, and STA/NTP that use
 * these are device-only and proven by the slice-0007 verify script (Scenarios C, D).
 */
#include <unity.h>

#include <cstdint>
#include <cstring>

#include "net/provisioning.h"

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

// --- validateCredentials: rejection boundaries ------------------------------

void test_rejects_empty_ssid(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::SsidEmpty),
                          static_cast<int>(validateCredentials("", "password", "deadbeefkey")));
}

void test_rejects_ssid_over_32_bytes(void) {
    const char* ssid33 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";  // 33 chars
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::SsidTooLong),
                          static_cast<int>(validateCredentials(ssid33, "password", "deadbeefkey")));
}

void test_rejects_passphrase_under_8_bytes(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::PassTooShort),
                          static_cast<int>(validateCredentials("net", "short", "deadbeefkey")));
}

void test_rejects_passphrase_over_63_bytes(void) {
    char pass64[65];
    for (int i = 0; i < 64; ++i) pass64[i] = 'x';
    pass64[64] = '\0';
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::PassTooLong),
                          static_cast<int>(validateCredentials("net", pass64, "deadbeefkey")));
}

void test_rejects_empty_key(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::KeyEmpty),
                          static_cast<int>(validateCredentials("net", "password", "")));
}

void test_rejects_null_fields(void) {
    // A null field is treated as empty, not dereferenced — the portal must never crash on one.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::SsidEmpty),
                          static_cast<int>(validateCredentials(nullptr, "password", "key")));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::KeyEmpty),
                          static_cast<int>(validateCredentials("net", "password", nullptr)));
}

// --- validateCredentials: accepting cases ----------------------------------

void test_accepts_wpa2_triad(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::None),
                          static_cast<int>(validateCredentials("home-net", "correcthorse", "abc123")));
}

void test_accepts_open_network_empty_passphrase(void) {
    // Empty passphrase = an open network, which is valid; only a non-empty short one is rejected.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::None),
                          static_cast<int>(validateCredentials("open-net", "", "abc123")));
}

void test_accepts_boundary_lengths(void) {
    char ssid32[33];
    for (int i = 0; i < 32; ++i) ssid32[i] = 's';
    ssid32[32] = '\0';
    char pass63[64];
    for (int i = 0; i < 63; ++i) pass63[i] = 'p';
    pass63[63] = '\0';
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::None),
                          static_cast<int>(validateCredentials(ssid32, pass63, "k")));
    // And the 8-byte passphrase floor is inclusive.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::None),
                          static_cast<int>(validateCredentials("net", "12345678", "k")));
}

// --- isUsableWebhookUrl: the optional push endpoint (ADR-0023) ---------------

void test_webhook_url_empty_or_null_is_unusable(void) {
    // Empty/absent means push is simply off — unusable, but NOT a failure that gates boot.
    TEST_ASSERT_FALSE(isUsableWebhookUrl(""));
    TEST_ASSERT_FALSE(isUsableWebhookUrl(nullptr));
}

void test_webhook_url_requires_https(void) {
    // A plaintext http:// endpoint is refused so an alert is never sent unencrypted (§3).
    TEST_ASSERT_FALSE(isUsableWebhookUrl("http://ntfy.sh/topic"));
    TEST_ASSERT_FALSE(isUsableWebhookUrl("ntfy.sh/topic"));
}

void test_webhook_url_rejects_overlong(void) {
    char url[kMaxWebhookUrlLen + 10];
    const char* prefix = "https://ntfy.sh/";
    const size_t plen = strlen(prefix);
    memcpy(url, prefix, plen);
    for (size_t i = plen; i < sizeof(url) - 1; ++i) url[i] = 'x';  // past kMaxWebhookUrlLen.
    url[sizeof(url) - 1] = '\0';
    TEST_ASSERT_FALSE(isUsableWebhookUrl(url));
}

void test_webhook_url_accepts_well_formed_https(void) {
    TEST_ASSERT_TRUE(isUsableWebhookUrl("https://ntfy.sh/my-sapper"));
    TEST_ASSERT_TRUE(isUsableWebhookUrl("https://discord.com/api/webhooks/1/abc"));
}

void test_webhook_url_does_not_gate_the_triad(void) {
    // The webhook is validated separately: a bad webhook URL must never turn a good WiFi/wpa-sec triad
    // unusable (which would send the device to the portal). validateCredentials ignores it entirely.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::None),
                          static_cast<int>(validateCredentials("home-net", "correcthorse", "abc123")));
    TEST_ASSERT_FALSE(isUsableWebhookUrl("http://not-tls/hook"));  // the URL is bad,
    // but the triad above still validated — the two checks are independent.
}

// --- decideBootPhase: every branch ------------------------------------------

void test_gate_valid_creds_go_to_station(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::StationConnect),
                          static_cast<int>(decideBootPhase(true, 0, false)));
}

void test_gate_absent_or_invalid_creds_open_portal(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(false, 0, false)));
}

void test_gate_boot_hold_on_provisioned_enters_maintenance(void) {
    // ADR-0039 decision 2: the button signal was repurposed. On a *provisioned* device a BOOT-hold now
    // opens Maintenance (the results dashboard) rather than the setup portal, and it outranks the STA
    // path even with no failures.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Maintenance),
                          static_cast<int>(decideBootPhase(true, 0, true)));
}

void test_gate_boot_hold_on_unprovisioned_still_opens_portal(void) {
    // There is nothing to maintain and no network to join, so the button is ignored: a first-boot
    // BOOT-hold still lands on the setup portal (ADR-0039 decision 2, precedence #1).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(false, 0, true)));
}

void test_gate_retry_budget_boundary(void) {
    // One below the budget still tries STA; at the budget it falls back to the portal. (No button.)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::StationConnect),
                          static_cast<int>(decideBootPhase(true, kStaRetryBudget - 1, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(true, kStaRetryBudget, false)));
}

// --- isUsableMaintenancePass: the optional hardened-AP passphrase (ADR-0039 #6) --------------------

void test_maintenance_pass_empty_or_null_is_usable(void) {
    // Empty/absent means "fall back to the published default AP password" — usable, not a failure.
    TEST_ASSERT_TRUE(isUsableMaintenancePass(""));
    TEST_ASSERT_TRUE(isUsableMaintenancePass(nullptr));
}

void test_maintenance_pass_rejects_below_wpa2_minimum(void) {
    // A 1..7-char value is refused so the save fails loud, never handed to WiFi.softAP() to drop.
    TEST_ASSERT_FALSE(isUsableMaintenancePass("short"));    // 5 chars
    TEST_ASSERT_FALSE(isUsableMaintenancePass("1234567"));  // 7 chars
}

void test_maintenance_pass_accepts_wpa2_length_window(void) {
    TEST_ASSERT_TRUE(isUsableMaintenancePass("12345678"));  // 8-char floor is inclusive.
    char pass63[64];
    for (int i = 0; i < 63; ++i) pass63[i] = 'p';
    pass63[63] = '\0';
    TEST_ASSERT_TRUE(isUsableMaintenancePass(pass63));      // 63-char ceiling is inclusive.
}

void test_maintenance_pass_rejects_overlong(void) {
    char pass64[65];
    for (int i = 0; i < 64; ++i) pass64[i] = 'p';
    pass64[64] = '\0';
    TEST_ASSERT_FALSE(isUsableMaintenancePass(pass64));     // past the WPA2 max.
}

// --- formatSoftApSsid -------------------------------------------------------

void test_softap_ssid_from_last_two_mac_bytes(void) {
    const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0x11, 0xAB, 0xCD};
    char ssid[kSoftApSsidBufSize];
    formatSoftApSsid(mac, ssid);
    TEST_ASSERT_EQUAL_STRING("Sapper-ABCD", ssid);
}

void test_softap_ssid_zero_pads(void) {
    const uint8_t mac[6] = {0, 0, 0, 0, 0x0A, 0x05};
    char ssid[kSoftApSsidBufSize];
    formatSoftApSsid(mac, ssid);
    TEST_ASSERT_EQUAL_STRING("Sapper-0A05", ssid);
}

// The Maintenance SoftAP carries the "Maint" tag so it is never confused with the provisioning portal
// (ADR-0055). From the same MAC the two names must differ — a regression to reusing formatSoftApSsid
// would make this equal "Sapper-ABCD" and fail.
void test_maintenance_softap_ssid_is_tagged_and_distinct(void) {
    const uint8_t mac[6] = {0x24, 0x6f, 0x28, 0x11, 0xAB, 0xCD};
    char maintSsid[kMaintApSsidBufSize];
    formatMaintenanceApSsid(mac, maintSsid);
    TEST_ASSERT_EQUAL_STRING("Sapper-Maint-ABCD", maintSsid);

    char provSsid[kSoftApSsidBufSize];
    formatSoftApSsid(mac, provSsid);
    TEST_ASSERT_EQUAL_STRING("Sapper-ABCD", provSsid);  // provisioning name unchanged (ADR-0006 #5)
    TEST_ASSERT_TRUE(std::strcmp(maintSsid, provSsid) != 0);  // distinguishable
}

// --- phaseLabel: the serial [STATE] contract ---------------------------------

void test_phase_labels_match_serial_contract(void) {
    // These exact tokens are what the slice-0007 verify script greps for; pin them so a rename
    // that would break the device verify fails here first, on the host lane.
    TEST_ASSERT_EQUAL_STRING("provisioning", phaseLabel(Phase::Provisioning));
    TEST_ASSERT_EQUAL_STRING("station_connect", phaseLabel(Phase::StationConnect));
    TEST_ASSERT_EQUAL_STRING("time_sync", phaseLabel(Phase::TimeSync));
    TEST_ASSERT_EQUAL_STRING("ready", phaseLabel(Phase::Ready));
    TEST_ASSERT_EQUAL_STRING("maintenance", phaseLabel(Phase::Maintenance));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_rejects_empty_ssid);
    RUN_TEST(test_rejects_ssid_over_32_bytes);
    RUN_TEST(test_rejects_passphrase_under_8_bytes);
    RUN_TEST(test_rejects_passphrase_over_63_bytes);
    RUN_TEST(test_rejects_empty_key);
    RUN_TEST(test_rejects_null_fields);
    RUN_TEST(test_accepts_wpa2_triad);
    RUN_TEST(test_accepts_open_network_empty_passphrase);
    RUN_TEST(test_accepts_boundary_lengths);
    RUN_TEST(test_webhook_url_empty_or_null_is_unusable);
    RUN_TEST(test_webhook_url_requires_https);
    RUN_TEST(test_webhook_url_rejects_overlong);
    RUN_TEST(test_webhook_url_accepts_well_formed_https);
    RUN_TEST(test_webhook_url_does_not_gate_the_triad);
    RUN_TEST(test_gate_valid_creds_go_to_station);
    RUN_TEST(test_gate_absent_or_invalid_creds_open_portal);
    RUN_TEST(test_gate_boot_hold_on_provisioned_enters_maintenance);
    RUN_TEST(test_gate_boot_hold_on_unprovisioned_still_opens_portal);
    RUN_TEST(test_gate_retry_budget_boundary);
    RUN_TEST(test_maintenance_pass_empty_or_null_is_usable);
    RUN_TEST(test_maintenance_pass_rejects_below_wpa2_minimum);
    RUN_TEST(test_maintenance_pass_accepts_wpa2_length_window);
    RUN_TEST(test_maintenance_pass_rejects_overlong);
    RUN_TEST(test_softap_ssid_from_last_two_mac_bytes);
    RUN_TEST(test_softap_ssid_zero_pads);
    RUN_TEST(test_maintenance_softap_ssid_is_tagged_and_distinct);
    RUN_TEST(test_phase_labels_match_serial_contract);
    return UNITY_END();
}
