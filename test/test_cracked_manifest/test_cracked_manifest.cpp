/**
 * @file test_cracked_manifest.cpp
 * @brief Native unit tests for the pure per-BSSID cracked manifest (ADR-0019 decisions #4, #5;
 *        slice-0020 Scenarios C, D).
 *
 * Proves the mirror policy against the in-RAM fake store: a known result is a no-op, a rotated PSK is
 * an in-place update reported Changed, the manifest is bounded and evicts the oldest crack, and
 * begin()/flush() round-trip the entries and the fresh flag and fail loud on a store error.
 */
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "net/cracked_manifest.h"
#include "support/fake_cracked_store.h"

using namespace sapper;
using sapper_test::FakeCrackedStore;

void setUp(void) {}
void tearDown(void) {}

// Build a result whose BSSID is index-tagged (last two octets = @p tag) with the given essid/password.
// A 16-bit tag gives enough distinct BSSIDs to overfill the manifest by one in the eviction test.
static CrackedResult makeResult(uint16_t tag, const char* essid, const char* password) {
    CrackedResult r;
    r.bssid[0] = 0xaa;
    r.bssid[4] = static_cast<uint8_t>(tag >> 8);
    r.bssid[5] = static_cast<uint8_t>(tag & 0xff);
    std::strncpy(r.essid, essid, kCrackedEssidCap - 1);
    std::strncpy(r.password, password, kCrackedPasswordCap - 1);
    return r;
}

void test_apply_new_bssid_is_new_and_stored(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());

    TEST_ASSERT_EQUAL(static_cast<int>(ApplyOutcome::New),
                      static_cast<int>(manifest.apply(makeResult(1, "Net1", "pw1"), 1000)));
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());
    const uint8_t bssid[6] = {0xaa, 0, 0, 0, 0, 1};
    const CrackedEntry* e = manifest.find(bssid);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("pw1", e->result.password);
    TEST_ASSERT_EQUAL_UINT32(1000, e->firstSeenCrackedMs);
}

void test_reapplying_same_result_is_unchanged_noop(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    manifest.apply(makeResult(1, "Net1", "pw1"), 1000);

    // Scenario C: the same BSSID+password again changes nothing and does not touch first-seen.
    TEST_ASSERT_EQUAL(static_cast<int>(ApplyOutcome::Unchanged),
                      static_cast<int>(manifest.apply(makeResult(1, "Net1", "pw1"), 5000)));
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());
    const uint8_t bssid[6] = {0xaa, 0, 0, 0, 0, 1};
    TEST_ASSERT_EQUAL_UINT32(1000, manifest.find(bssid)->firstSeenCrackedMs);
}

void test_changed_password_updates_in_place_and_reports_changed(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    manifest.apply(makeResult(1, "Net1", "oldpw"), 1000);

    // Scenario D: a rotated PSK is one entry updated, reported Changed, first-seen preserved.
    TEST_ASSERT_EQUAL(static_cast<int>(ApplyOutcome::Changed),
                      static_cast<int>(manifest.apply(makeResult(1, "Net1", "newpw"), 9000)));
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());
    const uint8_t bssid[6] = {0xaa, 0, 0, 0, 0, 1};
    const CrackedEntry* e = manifest.find(bssid);
    TEST_ASSERT_EQUAL_STRING("newpw", e->result.password);
    TEST_ASSERT_EQUAL_UINT32(1000, e->firstSeenCrackedMs);  // AP first-seen unchanged by a re-crack.
}

void test_bound_is_enforced_and_oldest_is_evicted(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());

    // Fill to the bound; entry i has firstSeenCracked = (i+1)*10, so tag 0 is the oldest.
    for (size_t i = 0; i < kMaxCrackedEntries; ++i) {
        char pw[8];
        std::snprintf(pw, sizeof(pw), "pw%zu", i);
        manifest.apply(makeResult(static_cast<uint16_t>(i), "Net", pw), static_cast<uint32_t>((i + 1) * 10));
    }
    TEST_ASSERT_EQUAL_size_t(kMaxCrackedEntries, manifest.size());

    // A brand-new BSSID (tag kMaxCrackedEntries, outside the filled range) must evict the oldest
    // (tag 0), keeping the count at the bound.
    const uint16_t newTag = static_cast<uint16_t>(kMaxCrackedEntries);
    manifest.apply(makeResult(newTag, "NewNet", "newpw"), 999999);
    TEST_ASSERT_EQUAL_size_t(kMaxCrackedEntries, manifest.size());

    const uint8_t oldest[6] = {0xaa, 0, 0, 0, 0, 0};
    TEST_ASSERT_NULL(manifest.find(oldest));  // evicted
    const uint8_t added[6] = {0xaa, 0, 0, 0, static_cast<uint8_t>(newTag >> 8),
                              static_cast<uint8_t>(newTag & 0xff)};
    TEST_ASSERT_NOT_NULL(manifest.find(added));  // kept
}

void test_begin_loads_persisted_entries_and_fresh_flag(void) {
    FakeCrackedStore store;
    CrackedEntry seeded;
    seeded.result = makeResult(7, "Seeded", "seedpw");
    seeded.firstSeenCrackedMs = 4242;
    store.seed({seeded});  // seed() marks the store non-fresh.

    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());
    TEST_ASSERT_FALSE(manifest.isFresh());
    const uint8_t bssid[6] = {0xaa, 0, 0, 0, 0, 7};
    TEST_ASSERT_EQUAL_STRING("seedpw", manifest.find(bssid)->result.password);
}

void test_begin_fails_loud_on_store_error(void) {
    FakeCrackedStore store;
    store.failLoad = true;
    CrackedManifest manifest(store);
    TEST_ASSERT_FALSE(manifest.begin());
}

void test_flush_persists_entries_and_fresh_flag(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    TEST_ASSERT_TRUE(store.fresh());  // fresh until the first flush clears it.

    manifest.apply(makeResult(1, "Net1", "pw1"), 1000);
    manifest.clearFresh();
    TEST_ASSERT_TRUE(manifest.flush());

    TEST_ASSERT_EQUAL_size_t(1, store.size());
    TEST_ASSERT_FALSE(store.fresh());
    TEST_ASSERT_EQUAL_INT(1, store.saveCount);
}

void test_flush_fails_loud_on_store_error(void) {
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    manifest.apply(makeResult(1, "Net1", "pw1"), 1000);
    store.failSave = true;
    TEST_ASSERT_FALSE(manifest.flush());
}

void test_entry_at_enumerates_and_bounds(void) {
    // The Maintenance dashboard (ADR-0039) enumerates the whole account through entryAt() rather than
    // parsing the store; an out-of-range index returns nullptr, never a stale slot past the count.
    FakeCrackedStore store;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    manifest.apply(makeResult(1, "Net1", "pw1"), 1000);
    manifest.apply(makeResult(2, "Net2", "pw2"), 2000);

    TEST_ASSERT_EQUAL_size_t(2, manifest.size());
    TEST_ASSERT_NOT_NULL(manifest.entryAt(0));
    TEST_ASSERT_NOT_NULL(manifest.entryAt(1));
    // The two entries are the two applied results (order is insertion order here).
    TEST_ASSERT_EQUAL_STRING("pw1", manifest.entryAt(0)->result.password);
    TEST_ASSERT_EQUAL_STRING("pw2", manifest.entryAt(1)->result.password);
    TEST_ASSERT_NULL(manifest.entryAt(2));  // past the count.
    TEST_ASSERT_NULL(manifest.entryAt(kMaxCrackedEntries));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_apply_new_bssid_is_new_and_stored);
    RUN_TEST(test_entry_at_enumerates_and_bounds);
    RUN_TEST(test_reapplying_same_result_is_unchanged_noop);
    RUN_TEST(test_changed_password_updates_in_place_and_reports_changed);
    RUN_TEST(test_bound_is_enforced_and_oldest_is_evicted);
    RUN_TEST(test_begin_loads_persisted_entries_and_fresh_flag);
    RUN_TEST(test_begin_fails_loud_on_store_error);
    RUN_TEST(test_flush_persists_entries_and_fresh_flag);
    RUN_TEST(test_flush_fails_loud_on_store_error);
    return UNITY_END();
}
