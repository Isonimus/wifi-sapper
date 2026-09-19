/**
 * @file test_capture_queue.cpp
 * @brief Native unit tests for the bounded, per-BSSID-deduplicated retry queue (slice-0018
 *        Scenario E, ADR-0017 decision #4).
 *
 * Drives the pure CaptureQueue through an in-RAM FakeCaptureStore: a real handshake is serialized to
 * a pcap and offered, and the store is asserted to enforce the bound, keep at most one pending entry
 * per BSSID (the newer, more-complete capture winning), evict the oldest on overflow, and fail loud
 * on a store error. No radio, no filesystem.
 */
#include <unity.h>

#include <array>
#include <cstring>

#include "net/capture_queue.h"
#include "net/handshake_collector.h"

#include "../support/fake_capture_store.h"
#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;
using sapper_test::FakeCaptureStore;

static const uint8_t kClient[6] = {0x06, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};

void setUp(void) {}
void tearDown(void) {}

/// Build a wpa-sec-valid handshake (beacon + M1 + M2) for @p bssid, optionally adding M3+M4 so a
/// "more complete" later capture can be distinguished by its larger serialized pcap.
static CapturedHandshake makeHandshake(const uint8_t bssid[6], bool complete) {
    HandshakeCollector collector(bssid, 6);
    const std::vector<uint8_t> beacon = buildBeacon(bssid, "Net", 6);
    collector.ingest(beacon.data(), static_cast<uint16_t>(beacon.size()));
    const std::vector<uint8_t> m1 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM1, true);
    collector.ingest(m1.data(), static_cast<uint16_t>(m1.size()));
    const std::vector<uint8_t> m2 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM2, false);
    collector.ingest(m2.data(), static_cast<uint16_t>(m2.size()));
    if (complete) {
        const std::vector<uint8_t> m3 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM3, true);
        collector.ingest(m3.data(), static_cast<uint16_t>(m3.size()));
        const std::vector<uint8_t> m4 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM4, false);
        collector.ingest(m4.data(), static_cast<uint16_t>(m4.size()));
    }
    return collector.handshake();
}

/// A distinct BSSID per index, so a test can fill the queue with many different APs.
static std::array<uint8_t, 6> bssidN(uint8_t n) {
    return {0x02, 0x00, 0x00, 0x00, 0x00, n};
}

void test_a_new_capture_is_stored_once(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Stored),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), false))));
    TEST_ASSERT_EQUAL_UINT32(1, store.size());
    TEST_ASSERT_EQUAL_UINT32(1, store.countForBssid(ap.data()));
    TEST_ASSERT_TRUE(store.entries()[0].bytes.size() > kPcapGlobalHeaderLen);  // a real pcap landed.
}

void test_an_invalid_handshake_is_not_stored(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    // Beacon + M1 only — not wpa-sec-valid (needs M2), so nothing serializable to store.
    HandshakeCollector collector(ap.data(), 6);
    const std::vector<uint8_t> beacon = buildBeacon(ap.data(), "Net", 6);
    collector.ingest(beacon.data(), static_cast<uint16_t>(beacon.size()));
    const std::vector<uint8_t> m1 = buildEapol(ap.data(), kClient, sapper_test::kKeyInfoM1, true);
    collector.ingest(m1.data(), static_cast<uint16_t>(m1.size()));

    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::NotUploadable),
                      static_cast<int>(queue.offer(collector.handshake())));
    TEST_ASSERT_EQUAL_UINT32(0, store.size());
}

void test_same_bssid_replaces_and_keeps_the_newer_capture(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Stored),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), /*complete=*/false))));
    const uint32_t firstId = store.entries()[0].id;
    const size_t firstLen = store.entries()[0].bytes.size();

    // A later, more-complete capture (M1-M4) for the same BSSID replaces the pending one.
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Replaced),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), /*complete=*/true))));
    TEST_ASSERT_EQUAL_UINT32(1, store.size());                 // count unchanged — not two entries.
    TEST_ASSERT_EQUAL_UINT32(1, store.countForBssid(ap.data()));
    TEST_ASSERT_NOT_EQUAL(firstId, store.entries()[0].id);     // the old entry is gone,
    TEST_ASSERT_TRUE(store.entries()[0].bytes.size() > firstLen);  // and the newer (larger) pcap kept.
}

void test_full_queue_evicts_the_oldest_on_a_new_bssid(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);

    // Fill to the bound with distinct BSSIDs; the first offered is the oldest.
    for (uint8_t i = 0; i < kMaxQueuedCaptures; ++i) {
        const auto ap = bssidN(i);
        TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Stored),
                          static_cast<int>(queue.offer(makeHandshake(ap.data(), false))));
    }
    TEST_ASSERT_EQUAL_UINT32(kMaxQueuedCaptures, store.size());
    const auto oldest = bssidN(0);
    const auto newest = bssidN(static_cast<uint8_t>(kMaxQueuedCaptures));

    // One more distinct BSSID: the count stays at the bound and the oldest is the one dropped.
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Evicted),
                      static_cast<int>(queue.offer(makeHandshake(newest.data(), false))));
    TEST_ASSERT_EQUAL_UINT32(kMaxQueuedCaptures, store.size());
    TEST_ASSERT_EQUAL_UINT32(0, store.countForBssid(oldest.data()));   // evicted,
    TEST_ASSERT_EQUAL_UINT32(1, store.countForBssid(newest.data()));   // newest kept.
}

void test_a_store_failure_is_reported_not_swallowed(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    store.failEnqueue = true;
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::StoreError),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), false))));
    TEST_ASSERT_EQUAL_UINT32(0, store.size());  // nothing persisted; caller must not assume success.
}

void test_a_reconcile_remove_failure_is_reported_and_keeps_the_new_capture(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    // One capture pending for this BSSID; a second offer for it must remove the old during reconcile.
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Stored),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), false))));

    // The store can persist the new capture but cannot delete the old one during dedup.
    store.failRemove = true;
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::StoreError),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), true))));

    // The failure is surfaced (StoreError, not Replaced) and the fresh capture still stands — it was
    // persisted before the reconcile — so the newest, most-complete capture is never lost to a tidy-up
    // failure; the stale duplicate remains to be healed on a later offer/drain.
    TEST_ASSERT_EQUAL_UINT32(2, store.size());
    TEST_ASSERT_EQUAL_UINT32(2, store.countForBssid(ap.data()));
}

void test_a_list_failure_after_persist_still_keeps_the_capture(void) {
    FakeCaptureStore store;
    CaptureQueue queue(store);
    const auto ap = bssidN(1);

    // The store can persist the new capture but cannot list the queue for reconcile. Because the
    // capture is persisted *first* (ADR-0017 decision #4), a listing failure surfaces StoreError yet
    // must never discard the freshest capture — the exact loss the "persist first" guarantee forbids.
    // (Before that fix, offer() listed before enqueuing, so this same failure dropped the capture.)
    store.failList = true;
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::StoreError),
                      static_cast<int>(queue.offer(makeHandshake(ap.data(), true))));
    TEST_ASSERT_EQUAL_UINT32(1, store.size());  // persisted despite the reconcile listing failing.
    TEST_ASSERT_EQUAL_UINT32(1, store.countForBssid(ap.data()));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_a_new_capture_is_stored_once);
    RUN_TEST(test_an_invalid_handshake_is_not_stored);
    RUN_TEST(test_same_bssid_replaces_and_keeps_the_newer_capture);
    RUN_TEST(test_full_queue_evicts_the_oldest_on_a_new_bssid);
    RUN_TEST(test_a_store_failure_is_reported_not_swallowed);
    RUN_TEST(test_a_reconcile_remove_failure_is_reported_and_keeps_the_new_capture);
    RUN_TEST(test_a_list_failure_after_persist_still_keeps_the_capture);
    return UNITY_END();
}
