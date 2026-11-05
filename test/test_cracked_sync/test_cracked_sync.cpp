/**
 * @file test_cracked_sync.cpp
 * @brief Native unit tests for the pure sync orchestration (ADR-0019 decisions #5, #7; slice-0020
 *        Scenarios E, F, G, I).
 *
 * Proves the alert policy end to end over the fake fetcher, fake store, and a recording observer:
 * a fresh manifest seeds silently and summarises (E); steady state announces only new/changed cracks
 * (F); a first real crack into an empty non-fresh manifest is announced, not summarised (G); and a
 * transport failure or a garbage body leaves the manifest untouched and announces nothing (I).
 */
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/event_bus.h"
#include "net/cracked_sync.h"
#include "support/fake_cracked_fetcher.h"
#include "support/fake_cracked_store.h"
#include "support/recording_event_sink.h"

using namespace sapper;
using sapper_test::FakeCrackedFetcher;
using sapper_test::FakeCrackedStore;
using sapper_test::RecordingEventSink;

void setUp(void) {}
void tearDown(void) {}

// A wpa-sec download line for a BSSID tagged by its last octet, with the given essid/password.
static std::string lineFor(uint8_t tag, const char* essid, const char* password) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "aabbccddee%02x:001122334455:%s:%s", tag, essid, password);
    return std::string(buf);
}

// A pwncrack potfile line for the same BSSID tagging: BSSID is field index 1, essid index 3, password
// index 4 (ADR-0109 #5). Field 0 is an auxiliary name the parser ignores; field 2 is filler.
static std::string pwncrackLineFor(uint8_t tag, const char* essid, const char* password) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "name:aabbccddee%02x:x:%s:%s", tag, essid, password);
    return std::string(buf);
}

// A persisted manifest entry for the same BSSID tagging, matching lineFor().
static CrackedEntry entryFor(uint8_t tag, const char* essid, const char* password, uint32_t firstSeen) {
    CrackedEntry e;
    const uint8_t bssid[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, tag};
    std::memcpy(e.result.bssid, bssid, 6);
    std::strncpy(e.result.essid, essid, kCrackedEssidCap - 1);
    std::strncpy(e.result.password, password, kCrackedPasswordCap - 1);
    e.firstSeenCrackedMs = firstSeen;
    return e;
}

void test_first_sync_of_fresh_manifest_seeds_silently_and_summarises(void) {
    // Scenario E.
    FakeCrackedStore store;  // fresh by default.
    FakeCrackedFetcher fetcher;
    fetcher.lines = {lineFor(1, "Alpha", "pw1"), lineFor(2, "Bravo", "pw2"), lineFor(3, "Charlie", "pw3")};
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "SECRETKEY", bus);

    const SyncOutcome out = sync.runSync(1000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_TRUE(out.firstSync);
    TEST_ASSERT_EQUAL_size_t(3, out.downloaded);
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());  // seeded silently
    TEST_ASSERT_EQUAL_INT(1, observer.summaries);               // exactly one summary
    TEST_ASSERT_EQUAL_size_t(3, observer.lastSummaryCount);
    TEST_ASSERT_EQUAL_size_t(3, manifest.size());
    TEST_ASSERT_FALSE(store.fresh());          // flag cleared and persisted
    TEST_ASSERT_EQUAL_INT(1, store.saveCount); // one write for the whole sync
    TEST_ASSERT_EQUAL_STRING("SECRETKEY", fetcher.lastKey.c_str());
}

void test_empty_account_first_sync_summarises_zero_and_clears_fresh(void) {
    // The legit-empty case: no results, no malformed — a success that clears fresh (part of Scenario E).
    FakeCrackedStore store;
    FakeCrackedFetcher fetcher;  // Ok, no lines.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(1000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_EQUAL_INT(1, observer.summaries);
    TEST_ASSERT_EQUAL_size_t(0, observer.lastSummaryCount);
    TEST_ASSERT_FALSE(store.fresh());
}

void test_steady_state_announces_only_new_and_changed(void) {
    // Scenario F.
    FakeCrackedStore store;
    store.seed({entryFor(1, "Alpha", "pw1", 100), entryFor(2, "Bravo", "pw2", 200)});
    FakeCrackedFetcher fetcher;
    fetcher.lines = {
        lineFor(1, "Alpha", "pw1"),       // unchanged — silent
        lineFor(2, "Bravo", "pw2-new"),   // changed password — announced
        lineFor(3, "Charlie", "pw3"),     // brand-new BSSID — announced
    };
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    TEST_ASSERT_FALSE(manifest.isFresh());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(5000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_FALSE(out.firstSync);
    TEST_ASSERT_EQUAL_size_t(2, observer.newPasswords.size());  // changed + new, not the unchanged one
    TEST_ASSERT_EQUAL_INT(0, observer.summaries);
    TEST_ASSERT_EQUAL_size_t(2, out.newPasswords);
    TEST_ASSERT_EQUAL_size_t(3, manifest.size());
}

void test_first_real_crack_into_empty_non_fresh_manifest_is_announced(void) {
    // Scenario G: empty but already-synced (non-fresh) manifest — the first real crack must alert, not
    // be misfiled as backlog. This is why "fresh" is a persisted flag, not "manifest empty".
    FakeCrackedStore store;
    store.seed({});  // seed() marks the store non-fresh, with no entries.
    FakeCrackedFetcher fetcher;
    fetcher.lines = {lineFor(9, "Zulu", "pw9")};
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    TEST_ASSERT_FALSE(manifest.isFresh());
    TEST_ASSERT_EQUAL_size_t(0, manifest.size());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(7000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_EQUAL_size_t(1, observer.newPasswords.size());  // announced
    TEST_ASSERT_EQUAL_INT(0, observer.summaries);               // not a summary
}

void test_transport_failure_leaves_manifest_untouched_and_silent(void) {
    // Scenario I (transport).
    FakeCrackedStore store;
    store.seed({entryFor(1, "Alpha", "pw1", 100)});
    FakeCrackedFetcher fetcher;
    fetcher.result = FetchResult::Transport;
    fetcher.lines = {lineFor(2, "Bravo", "pw2")};  // ignored — a transport failure yields nothing.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(9000);

    TEST_ASSERT_FALSE(out.ok);
    TEST_ASSERT_EQUAL(static_cast<int>(FetchResult::Transport), static_cast<int>(out.fetch));
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());
    TEST_ASSERT_EQUAL_INT(0, observer.summaries);
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());   // unchanged
    TEST_ASSERT_EQUAL_INT(0, store.saveCount);      // never persisted
    // One outcome event was still emitted so a surface learns the sync failed.
    TEST_ASSERT_EQUAL_size_t(1, observer.outcomes.size());
}

void test_garbage_body_is_a_failed_sync_that_changes_nothing(void) {
    // Scenario I (garbage/truncated body: a fetch that returns Ok but yields no parseable line).
    FakeCrackedStore store;
    store.seed({entryFor(1, "Alpha", "pw1", 100)});
    FakeCrackedFetcher fetcher;  // Ok, but every line is malformed.
    fetcher.lines = {"<html>error</html>", "not-a-result", "still-garbage"};
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(9000);

    TEST_ASSERT_FALSE(out.ok);
    TEST_ASSERT_EQUAL_size_t(0, out.downloaded);
    TEST_ASSERT_TRUE(out.malformed > 0);            // surfaced loud, not parsed-to-nothing silently
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());   // unchanged
    TEST_ASSERT_EQUAL_INT(0, store.saveCount);
}

void test_flush_failure_is_reported_and_announces_nothing(void) {
    // A store write failure after apply is surfaced (storeError), the sync is not ok, and no crack is
    // announced (persistence precedes announcement — quality bar §3).
    FakeCrackedStore store;
    store.seed({});  // non-fresh, empty.
    FakeCrackedFetcher fetcher;
    fetcher.lines = {lineFor(3, "Charlie", "pw3")};
    store.failSave = true;
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(fetcher, manifest, "K", bus);

    const SyncOutcome out = sync.runSync(9000);

    TEST_ASSERT_FALSE(out.ok);
    TEST_ASSERT_TRUE(out.storeError);
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());  // not announced — never persisted
}

// --- ADR-0109 #5: pwncrack as a second source into the one shared, backend-agnostic manifest. ---

void test_both_sources_feed_the_one_manifest_and_each_alerts_once(void) {
    // A new crack from wpa-sec AND a new crack from pwncrack both land in the same manifest and each
    // fires exactly one backend-agnostic NewPassword alert; no manifest/alert field is backend-aware.
    FakeCrackedStore store;
    store.seed({});  // non-fresh, empty: steady state, so new cracks alert rather than seed.
    FakeCrackedFetcher wpaSec;
    wpaSec.lines = {lineFor(1, "Alpha", "pw1")};
    FakeCrackedFetcher pwncrack;
    pwncrack.lines = {pwncrackLineFor(2, "Bravo", "pw2")};
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(wpaSec, manifest, "WK", bus);
    sync.setPwncrackSource(pwncrack, "PK");

    const SyncOutcome out = sync.runSync(1000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_TRUE(out.pwncrackRan);
    TEST_ASSERT_EQUAL_size_t(2, out.downloaded);               // one line from each source
    TEST_ASSERT_EQUAL_size_t(2, observer.newPasswords.size()); // one alert each, both announced
    TEST_ASSERT_EQUAL_size_t(2, manifest.size());
    TEST_ASSERT_EQUAL_STRING("WK", wpaSec.lastKey.c_str());
    TEST_ASSERT_EQUAL_STRING("PK", pwncrack.lastKey.c_str());
    // Both BSSIDs are present in the shared manifest.
    const uint8_t b1[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01};
    const uint8_t b2[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02};
    TEST_ASSERT_NOT_NULL(manifest.find(b1));
    TEST_ASSERT_NOT_NULL(manifest.find(b2));
}

void test_pwncrack_colon_password_survives_into_the_manifest(void) {
    // The pwncrack parser's verbatim-past-the-fourth-colon password reaches the manifest intact.
    FakeCrackedStore store;
    store.seed({});
    FakeCrackedFetcher wpaSec;  // Ok, empty.
    FakeCrackedFetcher pwncrack;
    pwncrack.lines = {pwncrackLineFor(7, "Golf", "p@ss:w0rd:!")};
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(wpaSec, manifest, "WK", bus);
    sync.setPwncrackSource(pwncrack, "PK");

    const SyncOutcome out = sync.runSync(2000);

    TEST_ASSERT_TRUE(out.ok);
    const uint8_t b7[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x07};
    const CrackedEntry* e = manifest.find(b7);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("p@ss:w0rd:!", e->result.password);
}

void test_pwncrack_failure_still_applies_wpasec_and_marks_for_retry(void) {
    // A flaky second backend must never block the primary's cracks: wpa-sec's crack still applies and is
    // announced, but the sync is not ok (so the scheduler retries next window to pick up pwncrack).
    FakeCrackedStore store;
    store.seed({});  // non-fresh, empty.
    FakeCrackedFetcher wpaSec;
    wpaSec.lines = {lineFor(1, "Alpha", "pw1")};
    FakeCrackedFetcher pwncrack;
    pwncrack.result = FetchResult::Transport;
    pwncrack.lines = {pwncrackLineFor(2, "Bravo", "pw2")};  // ignored: transport failure yields nothing.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(wpaSec, manifest, "WK", bus);
    sync.setPwncrackSource(pwncrack, "PK");

    const SyncOutcome out = sync.runSync(3000);

    TEST_ASSERT_FALSE(out.ok);                                       // not ok → scheduler retries
    TEST_ASSERT_EQUAL(static_cast<int>(FetchResult::Transport), static_cast<int>(out.pwncrackFetch));
    TEST_ASSERT_EQUAL(static_cast<int>(FetchResult::Ok), static_cast<int>(out.fetch));
    TEST_ASSERT_EQUAL_size_t(1, manifest.size());                   // wpa-sec's crack applied
    TEST_ASSERT_EQUAL_size_t(1, observer.newPasswords.size());      // and announced
    TEST_ASSERT_EQUAL_INT(1, store.saveCount);                      // persisted once
    const uint8_t b2[6] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02};
    TEST_ASSERT_NULL(manifest.find(b2));                            // pwncrack's partial body discarded
}

void test_partial_first_sync_stays_fresh_then_a_full_sync_seeds_and_summarises(void) {
    // A fresh manifest whose first sync has one source down seeds what it got SILENTLY and stays fresh
    // (no summary yet: the baseline is incomplete, ADR-0109 #5). A later full sync completes the
    // baseline: it seeds the rest silently and emits exactly one summary for the whole seeded set.
    FakeCrackedStore store;  // fresh.
    FakeCrackedFetcher wpaSec;
    wpaSec.lines = {lineFor(1, "Alpha", "pw1"), lineFor(2, "Bravo", "pw2")};
    FakeCrackedFetcher pwncrack;
    pwncrack.result = FetchResult::Transport;  // down on the first sync.
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(wpaSec, manifest, "WK", bus);
    sync.setPwncrackSource(pwncrack, "PK");

    const SyncOutcome first = sync.runSync(1000);
    TEST_ASSERT_FALSE(first.ok);                       // a source was down
    TEST_ASSERT_TRUE(first.firstSync);
    TEST_ASSERT_EQUAL_size_t(2, manifest.size());      // wpa-sec's backlog seeded
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());  // seeded silently
    TEST_ASSERT_EQUAL_INT(0, observer.summaries);      // no summary: baseline incomplete
    TEST_ASSERT_TRUE(store.fresh());                   // stays fresh for the next sync

    // Second sync: pwncrack recovers and brings its own backlog entry; wpa-sec re-sends its two.
    pwncrack.result = FetchResult::Ok;
    pwncrack.lines = {pwncrackLineFor(3, "Charlie", "pw3")};

    const SyncOutcome second = sync.runSync(2000);
    TEST_ASSERT_TRUE(second.ok);
    TEST_ASSERT_TRUE(second.firstSync);                // still a first sync: the baseline completes now
    TEST_ASSERT_EQUAL_size_t(3, manifest.size());      // all three seeded
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());  // still silent (first-sync seed)
    TEST_ASSERT_EQUAL_INT(1, observer.summaries);      // exactly one summary, for the completed baseline
    TEST_ASSERT_EQUAL_size_t(3, observer.lastSummaryCount);
    TEST_ASSERT_FALSE(store.fresh());                  // baseline established
}

void test_unset_pwncrack_source_is_byte_for_byte_the_wpasec_only_sync(void) {
    // With no pwncrack source wired, runSync behaves exactly as before: a wpa-sec-only device is
    // unchanged (ADR-0109 #5, optional-backend boundary).
    FakeCrackedStore store;
    store.seed({entryFor(1, "Alpha", "pw1", 100)});
    FakeCrackedFetcher wpaSec;
    wpaSec.lines = {lineFor(2, "Bravo", "pw2")};  // one brand-new crack
    CrackedManifest manifest(store);
    TEST_ASSERT_TRUE(manifest.begin());
    EventBus bus;
    RecordingEventSink observer;
    bus.subscribe(observer);
    CrackedSync sync(wpaSec, manifest, "WK", bus);  // no setPwncrackSource

    const SyncOutcome out = sync.runSync(5000);

    TEST_ASSERT_TRUE(out.ok);
    TEST_ASSERT_FALSE(out.pwncrackRan);
    TEST_ASSERT_EQUAL_size_t(1, observer.newPasswords.size());
    TEST_ASSERT_EQUAL_size_t(2, manifest.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_first_sync_of_fresh_manifest_seeds_silently_and_summarises);
    RUN_TEST(test_empty_account_first_sync_summarises_zero_and_clears_fresh);
    RUN_TEST(test_steady_state_announces_only_new_and_changed);
    RUN_TEST(test_first_real_crack_into_empty_non_fresh_manifest_is_announced);
    RUN_TEST(test_transport_failure_leaves_manifest_untouched_and_silent);
    RUN_TEST(test_garbage_body_is_a_failed_sync_that_changes_nothing);
    RUN_TEST(test_flush_failure_is_reported_and_announces_nothing);
    RUN_TEST(test_both_sources_feed_the_one_manifest_and_each_alerts_once);
    RUN_TEST(test_pwncrack_colon_password_survives_into_the_manifest);
    RUN_TEST(test_pwncrack_failure_still_applies_wpasec_and_marks_for_retry);
    RUN_TEST(test_partial_first_sync_stays_fresh_then_a_full_sync_seeds_and_summarises);
    RUN_TEST(test_unset_pwncrack_source_is_byte_for_byte_the_wpasec_only_sync);
    return UNITY_END();
}
