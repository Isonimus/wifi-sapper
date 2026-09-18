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

// --- decideBootPhase: every branch ------------------------------------------

void test_gate_valid_creds_go_to_station(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::StationConnect),
                          static_cast<int>(decideBootPhase(true, 0, false)));
}

void test_gate_absent_or_invalid_creds_open_portal(void) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(false, 0, false)));
}

void test_gate_reprovision_request_opens_portal(void) {
    // Even with valid creds and no failures, a re-provision request (GPIO/button hold) wins.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(true, 0, true)));
}

void test_gate_retry_budget_boundary(void) {
    // One below the budget still tries STA; at the budget it falls back to the portal.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::StationConnect),
                          static_cast<int>(decideBootPhase(true, kStaRetryBudget - 1, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Provisioning),
                          static_cast<int>(decideBootPhase(true, kStaRetryBudget, false)));
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
    RUN_TEST(test_gate_valid_creds_go_to_station);
    RUN_TEST(test_gate_absent_or_invalid_creds_open_portal);
    RUN_TEST(test_gate_reprovision_request_opens_portal);
    RUN_TEST(test_gate_retry_budget_boundary);
    RUN_TEST(test_softap_ssid_from_last_two_mac_bytes);
    RUN_TEST(test_softap_ssid_zero_pads);
    return UNITY_END();
}
