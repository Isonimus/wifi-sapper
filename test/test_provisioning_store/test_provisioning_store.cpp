/**
 * @file test_provisioning_store.cpp
 * @brief Native unit tests for the NVS store (ADR-0008) behind the fake <Preferences.h>.
 *
 * Proves the store's failure and stale-state behaviour that the hardware lane (Scenarios C, D)
 * cannot induce on demand: a partial write must never leave a mixed triad, and an invalid seed
 * must clear any credentials a prior flash left behind. Both are regressions for defects the
 * slice-0007 freeze adversarial pass found (ADR-0008).
 */
#include <unity.h>

#include <Preferences.h>  // the native fake: resetNvs(), failPutKey() (ADR-0008)

#include <cstring>

#include "net/provisioning_store.h"

using namespace sapper;

// The NVS key whose write we fault-inject. Must match the store's `wpasec_key` (the middle of the
// pass -> key -> ssid write order), so a failed middle write is what leaves a would-be mix.
static const char* const kMiddleWriteKey = "wpasec_key";

void setUp(void) { sapper_test::resetNvs(); }
void tearDown(void) { sapper_test::resetNvs(); }

static ProvisioningRecord makeRecord(const char* ssid, const char* pass, const char* key) {
    ProvisioningRecord record = {};
    std::snprintf(record.ssid, sizeof(record.ssid), "%s", ssid);
    std::snprintf(record.pass, sizeof(record.pass), "%s", pass);
    std::snprintf(record.key, sizeof(record.key), "%s", key);
    return record;
}

// --- persist / load round-trip ----------------------------------------------

void test_persist_then_load_roundtrips(void) {
    const ProvisioningRecord in = makeRecord("home-net", "correcthorse", "deadbeefkey");
    TEST_ASSERT_TRUE(persistProvisioning(in));

    ProvisioningRecord out = {};
    TEST_ASSERT_TRUE(loadProvisioning(out));
    TEST_ASSERT_EQUAL_STRING("home-net", out.ssid);
    TEST_ASSERT_EQUAL_STRING("correcthorse", out.pass);
    TEST_ASSERT_EQUAL_STRING("deadbeefkey", out.key);
}

void test_load_on_first_boot_is_not_provisioned(void) {
    // Nothing ever written: the read-only begin fails and load reports "not provisioned".
    ProvisioningRecord out = {};
    TEST_ASSERT_FALSE(loadProvisioning(out));
}

// --- finding #1: a partial write must wipe, never leave a mix ----------------

void test_partial_write_on_reprovision_wipes_instead_of_mixing(void) {
    // A device already provisioned with a good triad.
    TEST_ASSERT_TRUE(persistProvisioning(makeRecord("net-old", "passOldAAAA", "keyOldAAAA")));

    // Re-provision to a new triad, but the middle write (wpasec_key) fails mid-way.
    sapper_test::failPutKey() = kMiddleWriteKey;
    TEST_ASSERT_FALSE(persistProvisioning(makeRecord("net-new", "passNewBBBB", "keyNewBBBB")));
    sapper_test::failPutKey().clear();

    // The store must NOT now hold (net-old, passNewBBBB, keyOldAAAA) — a triad no one submitted
    // that still validates. A partial failure leaves nothing usable, so the next boot re-opens the
    // portal rather than associating with a Frankenstein credential set (ADR-0008).
    ProvisioningRecord out = {};
    TEST_ASSERT_FALSE(loadProvisioning(out));
}

// --- finding #2: an invalid seed clears stale credentials --------------------

void test_seed_invalid_clears_stale_triad(void) {
    // A real triad from a prior flash.
    TEST_ASSERT_TRUE(persistProvisioning(makeRecord("net-old", "passOldAAAA", "keyOldAAAA")));

    // A hooks build with no credential flags seeds an empty (invalid) triad.
    seedProvisioning(ProvisioningRecord{});

    // The store must reflect the (empty) seed, not inherit the stale creds: load reports "not
    // provisioned" so the boot gate routes to the portal (ADR-0008, fail loud).
    ProvisioningRecord out = {};
    TEST_ASSERT_FALSE(loadProvisioning(out));
}

void test_seed_valid_replaces_triad(void) {
    TEST_ASSERT_TRUE(persistProvisioning(makeRecord("net-old", "passOldAAAA", "keyOldAAAA")));

    seedProvisioning(makeRecord("net-seed", "passSeedCCC", "keySeedCCC"));

    ProvisioningRecord out = {};
    TEST_ASSERT_TRUE(loadProvisioning(out));
    TEST_ASSERT_EQUAL_STRING("net-seed", out.ssid);
    TEST_ASSERT_EQUAL_STRING("passSeedCCC", out.pass);
    TEST_ASSERT_EQUAL_STRING("keySeedCCC", out.key);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_persist_then_load_roundtrips);
    RUN_TEST(test_load_on_first_boot_is_not_provisioned);
    RUN_TEST(test_partial_write_on_reprovision_wipes_instead_of_mixing);
    RUN_TEST(test_seed_invalid_clears_stale_triad);
    RUN_TEST(test_seed_valid_replaces_triad);
    return UNITY_END();
}
