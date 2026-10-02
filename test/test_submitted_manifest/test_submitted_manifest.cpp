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

#include "net/reverification.h"  // kReverifyIntervalSec / kMinRealEpoch for the cadence tests (ADR-0072).
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

// --- Rotated-PSK re-verification cadence (ADR-0072, §4 #33). ---

/// A valid wall-clock epoch (well above the real-clock threshold) to key the cadence tests on.
static constexpr uint32_t kBaseEpoch = kMinRealEpoch + 1000000u;

// Scenario A: a submitted entry gone stale against the verification clock is reported not-submitted, so
// gate C re-enables it; one within the interval is still reported submitted.
void test_stale_submitted_entry_is_reverification_eligible(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    uint8_t x[6];
    bssidOf(1, x);
    m.setVerificationClock(kBaseEpoch);
    m.markSubmitted(x, 100);                        // verified at kBaseEpoch.
    TEST_ASSERT_TRUE(m.isSubmitted(x));             // fresh → still a member (skipped).

    m.setVerificationClock(kBaseEpoch + kReverifyIntervalSec - 1);
    TEST_ASSERT_TRUE(m.isSubmitted(x));             // one second inside the interval → still fresh.

    m.setVerificationClock(kBaseEpoch + kReverifyIntervalSec);
    TEST_ASSERT_FALSE(m.isSubmitted(x));            // interval elapsed → due, reported not-submitted.
}

// Scenario B: re-marking a held BSSID (a terminal re-upload) re-stamps the re-verification timer and
// re-suppresses it, while the uploadedAt eviction key is left untouched.
void test_remark_bumps_the_reverification_timer_keeping_uploadedAt(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    uint8_t x[6];
    bssidOf(1, x);
    m.setVerificationClock(kBaseEpoch);
    m.markSubmitted(x, 100);                        // uploadedAt=100, verified=kBaseEpoch.

    m.setVerificationClock(kBaseEpoch + kReverifyIntervalSec);
    TEST_ASSERT_FALSE(m.isSubmitted(x));            // stale → due for re-verification.

    m.markSubmitted(x, 777);                        // the re-upload re-marks at the current clock.
    TEST_ASSERT_TRUE(m.isSubmitted(x));             // re-suppressed: fresh again for another interval.
    TEST_ASSERT_EQUAL_UINT32(1, m.size());          // still one record (membership, not a second row).

    TEST_ASSERT_TRUE(m.flushSubmitted());
    TEST_ASSERT_EQUAL_UINT32(100, store.entries()[0].uploadedAt);  // eviction key unchanged (not 777).
    TEST_ASSERT_EQUAL_UINT32(kBaseEpoch + kReverifyIntervalSec,
                             store.entries()[0].lastVerifiedEpoch);  // re-verification timer bumped.
}

// Scenario D: before the first NTP sync (clock below kMinRealEpoch) the cadence is inert — every member is
// reported submitted exactly as before the cadence existed, so a clockless device never re-verifies.
void test_cadence_is_inert_without_a_real_clock(void) {
    FakeSubmittedStore store;
    SubmittedManifest m(store);
    TEST_ASSERT_TRUE(m.begin());

    uint8_t x[6];
    bssidOf(1, x);
    m.markSubmitted(x, 100);                        // marked with the default ~0 clock (no sync yet).
    TEST_ASSERT_TRUE(m.isSubmitted(x));             // member reported submitted (pre-cadence behaviour).

    m.setVerificationClock(kMinRealEpoch - 1);      // still below the real-clock threshold.
    TEST_ASSERT_TRUE(m.isSubmitted(x));             // never stale without a real clock.
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_marks_and_reads_membership);
    RUN_TEST(test_remark_is_idempotent_keeping_original_uploadedAt);
    RUN_TEST(test_round_trips_through_the_store);
    RUN_TEST(test_bounds_and_evicts_oldest);
    RUN_TEST(test_load_failure_is_surfaced);
    RUN_TEST(test_save_failure_is_surfaced);
    RUN_TEST(test_stale_submitted_entry_is_reverification_eligible);
    RUN_TEST(test_remark_bumps_the_reverification_timer_keeping_uploadedAt);
    RUN_TEST(test_cadence_is_inert_without_a_real_clock);
    return UNITY_END();
}
