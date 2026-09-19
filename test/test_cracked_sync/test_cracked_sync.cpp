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

#include "net/cracked_sync.h"
#include "support/fake_cracked_fetcher.h"
#include "support/fake_cracked_store.h"

using namespace sapper;
using sapper_test::FakeCrackedFetcher;
using sapper_test::FakeCrackedStore;

// Records every event the sync emits so a test can assert exactly what was announced.
class RecordingObserver : public SyncEventObserver {
public:
    std::vector<CrackedResult> newPasswords;
    int summaries = 0;
    size_t lastSummaryCount = 0;
    std::vector<SyncOutcome> outcomes;

    void onNewPassword(const CrackedResult& r) override { newPasswords.push_back(r); }
    void onFirstSyncSummary(size_t n) override { ++summaries; lastSummaryCount = n; }
    void onSyncOutcome(const SyncOutcome& o) override { outcomes.push_back(o); }
};

void setUp(void) {}
void tearDown(void) {}

// A download line for a BSSID tagged by its last octet, with the given essid/password.
static std::string lineFor(uint8_t tag, const char* essid, const char* password) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "aabbccddee%02x:001122334455:%s:%s", tag, essid, password);
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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "SECRETKEY", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

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
    RecordingObserver observer;
    CrackedSync sync(fetcher, manifest, "K", observer);

    const SyncOutcome out = sync.runSync(9000);

    TEST_ASSERT_FALSE(out.ok);
    TEST_ASSERT_TRUE(out.storeError);
    TEST_ASSERT_EQUAL_size_t(0, observer.newPasswords.size());  // not announced — never persisted
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
    return UNITY_END();
}
