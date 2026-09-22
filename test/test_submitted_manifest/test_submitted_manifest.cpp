/**
 * @file test_submitted_manifest.cpp
 * @brief Native unit tests for the pure submitted-to-wpa-sec set (slice-0066 Scenario E; ADR-0065).
 *
 * Drives SubmittedManifest against an in-RAM FakeSubmittedStore: membership, idempotent re-marking
 * (original uploadedAt kept), whole round-trip through the store, the bound + oldest-uploadedAt
 * eviction, and the fail-loud load/save paths. No filesystem.
 */
#include <unity.h>

#include <cstring>
#include <vector>

#include "net/submitted_manifest.h"

#include "../support/fake_submitted_store.h"

using namespace sapper;
using sapper_test::FakeSubmittedStore;

void setUp(void) {}
void tearDown(void) {}

/// A BSSID {0x02, 0, 0, 0, hi, lo} — two varying octets so >255 distinct APs are addressable.
static void bssidOf(uint16_t n, uint8_t out[6]) {
    out[0] = 0x02;
    out[1] = 0x00;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = static_cast<uint8_t>(n >> 8);
    out[5] = static_cast<uint8_t>(n & 0xFF);
}

void test_marks_and_reads_membership(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    uint8_t x[6];
    uint8_t y[6];
    bssidOf(1, x);
    bssidOf(2, y);
    TEST_ASSERT_FALSE(m.isSubmitted(x));  // nothing marked yet.

    m.markSubmitted(x, 100);
    TEST_ASSERT_TRUE(m.isSubmitted(x));   // X now a member,
    TEST_ASSERT_FALSE(m.isSubmitted(y));  // Y still not,
    TEST_ASSERT_EQUAL_UINT32(1, m.size());
}

void test_remark_is_idempotent_keeping_original_uploadedAt(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    uint8_t x[6];
    bssidOf(7, x);
    m.markSubmitted(x, 100);
    m.markSubmitted(x, 500);  // re-mark the same BSSID later.

    TEST_ASSERT_EQUAL_UINT32(1, m.size());  // still one entry — membership, not a second record.
    TEST_ASSERT_TRUE(m.flushSubmitted());
    TEST_ASSERT_EQUAL_UINT32(1, store.entries().size());
    TEST_ASSERT_EQUAL_UINT32(100, store.entries()[0].uploadedAt);  // original stamp kept, not 500.
}

void test_round_trips_through_the_store(void) {
    FakeSubmittedStore store;
    uint8_t x[6];
    bssidOf(9, x);
    {
        SubmittedManifest m(store);
        TEST_ASSERT_TRUE(m.begin());
        m.markSubmitted(x, 42);
        TEST_ASSERT_TRUE(m.flushSubmitted());
        TEST_ASSERT_EQUAL_INT(1, store.saveCount);  // one write.
    }
    // A fresh manifest over the same store reloads the persisted set — survives a "reboot".
    SubmittedManifest reloaded(store);
    TEST_ASSERT_TRUE(reloaded.begin());
    TEST_ASSERT_TRUE(reloaded.isSubmitted(x));
    TEST_ASSERT_EQUAL_UINT32(1, reloaded.size());
}

void test_bounds_and_evicts_oldest(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    // Fill exactly to the bound, oldest first (uploadedAt increasing with n).
    for (uint16_t n = 0; n < kMaxSubmittedEntries; ++n) {
        uint8_t b[6];
        bssidOf(n, b);
        m.markSubmitted(b, static_cast<uint32_t>(n) + 1);  // uploadedAt 1..kMax; n=0 is oldest.
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxSubmittedEntries, m.size());

    // One more distinct BSSID overflows the bound: the oldest (n=0) is evicted, count stays at the bound.
    uint8_t overflow[6];
    bssidOf(kMaxSubmittedEntries, overflow);
    m.markSubmitted(overflow, 100000);

    TEST_ASSERT_EQUAL_UINT32(kMaxSubmittedEntries, m.size());  // still bounded,
    uint8_t oldest[6];
    bssidOf(0, oldest);
    TEST_ASSERT_FALSE(m.isSubmitted(oldest));    // the oldest was evicted,
    TEST_ASSERT_TRUE(m.isSubmitted(overflow));   // the newcomer is in,
    uint8_t survivor[6];
    bssidOf(1, survivor);
    TEST_ASSERT_TRUE(m.isSubmitted(survivor));   // a non-oldest member survives.
}

void test_load_failure_is_surfaced(void) {
    FakeSubmittedStore store;
    store.failLoad = true;
    SubmittedManifest m(store);
    TEST_ASSERT_FALSE(m.begin());  // fail loud — never run gate C against unknown prior state.
}

void test_save_failure_is_surfaced(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());
    uint8_t x[6];
    bssidOf(3, x);
    m.markSubmitted(x, 1);
    store.failSave = true;
    TEST_ASSERT_FALSE(m.flushSubmitted());  // a store write failure is surfaced, not swallowed.
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_marks_and_reads_membership);
    RUN_TEST(test_remark_is_idempotent_keeping_original_uploadedAt);
    RUN_TEST(test_round_trips_through_the_store);
    RUN_TEST(test_bounds_and_evicts_oldest);
    RUN_TEST(test_load_failure_is_surfaced);
    RUN_TEST(test_save_failure_is_surfaced);
    return UNITY_END();
}
