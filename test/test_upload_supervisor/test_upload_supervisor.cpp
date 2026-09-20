/**
 * @file test_upload_supervisor.cpp
 * @brief Native unit tests for the batched-drain upload arbiter (slice-0018 Scenarios A-D, F;
 *        ADR-0017 decisions #2, #5, #7).
 *
 * Drives the pure UploadSupervisor against a real HuntEngine (over a FakeRadioSniffer, so stop/resume
 * is observed through the engine's phase), a real CaptureQueue over an in-RAM FakeCaptureStore, a
 * scripted FakeUploader, a FakeStationControl, and a fake clock (nowMs passed to tick). No radio, no
 * network. It proves: a capture below threshold is enqueued without pausing the hunt; a threshold
 * triggers one batched drain that uploads all once and resumes; a rejected upload is retried and the
 * backoff grows; a duplicate is a terminal success; and the backoff grows on failure and resets on
 * success.
 */
#include <unity.h>

#include <array>
#include <cstring>
#include <vector>

#include "core/event_bus.h"
#include "net/ap_registry.h"
#include "net/capture_queue.h"
#include "net/handshake_collector.h"
#include "net/hunt_engine.h"
#include "net/upload_supervisor.h"

#include "../support/fake_capture_store.h"
#include "../support/fake_radio_sniffer.h"
#include "../support/fake_station_control.h"
#include "../support/fake_sync_session.h"
#include "../support/fake_uploader.h"
#include "../support/fake_window_notifier.h"
#include "../support/frame_builders.h"
#include "../support/recording_event_sink.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;
using sapper_test::FakeCaptureStore;
using sapper_test::FakeRadioSniffer;
using sapper_test::FakeStationControl;
using sapper_test::FakeSyncSession;
using sapper_test::FakeUploader;

static const uint8_t kClient[6] = {0x06, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
static uint8_t kHopChannels[3] = {1, 6, 11};
static const char* kKey = "test-wpasec-key";

void setUp(void) {}
void tearDown(void) {}

static CapturedHandshake makeHandshake(const uint8_t bssid[6]) {
    HandshakeCollector collector(bssid, 6);
    const std::vector<uint8_t> beacon = buildBeacon(bssid, "Net", 6);
    collector.ingest(beacon.data(), static_cast<uint16_t>(beacon.size()));
    const std::vector<uint8_t> m1 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM1, true);
    collector.ingest(m1.data(), static_cast<uint16_t>(m1.size()));
    const std::vector<uint8_t> m2 = buildEapol(bssid, kClient, sapper_test::kKeyInfoM2, false);
    collector.ingest(m2.data(), static_cast<uint16_t>(m2.size()));
    return collector.handshake();
}

static std::array<uint8_t, 6> bssidN(uint8_t n) { return {0x02, 0x00, 0x00, 0x00, 0x00, n}; }

static bool isDiscovering(const HuntEngine& engine) {
    return engine.phase() == HuntEngine::Phase::Discovering;
}
static bool isIdle(const HuntEngine& engine) { return engine.phase() == HuntEngine::Phase::Idle; }

/// The wired engine+queue+supervisor, built in the one order that resolves the construction cycle:
/// relay first, engine observing the relay, supervisor holding the engine, relay aimed at supervisor.
struct Rig {
    FakeRadioSniffer sniffer;
    ApRegistry registry;
    CaptureReadyRelay relay;
    HuntEngine engine;
    FakeCaptureStore store;
    CaptureQueue queue;
    FakeUploader uploader;
    FakeStationControl station;
    UploadSupervisor supervisor;

    explicit Rig(const UploadSupervisorConfig& config, SyncSession* sync = nullptr,
                 EventBus* bus = nullptr)
        : engine(sniffer, registry, relay, kHopChannels, 3, HuntConfig{}),
          queue(store),
          supervisor(engine, queue, uploader, station, kKey, config, sync, bus) {
        relay.setTarget(supervisor);
    }

    /// Bring the appliance to its running steady state: engine hunting, supervisor seeded.
    void start(uint32_t nowMs) {
        TEST_ASSERT_TRUE(engine.begin(nowMs));
        supervisor.begin(nowMs);
    }
};

void test_capture_below_threshold_is_enqueued_without_pausing_the_hunt(void) {
    UploadSupervisorConfig config;
    config.drainThreshold = 3;
    config.maxDrainIntervalMs = 1000000;  // keep the time ceiling out of this test.
    Rig rig(config);
    rig.start(0);

    const auto ap = bssidN(1);
    rig.supervisor.onCaptureReady(makeHandshake(ap.data()));  // one capture, below the threshold of 3.
    rig.supervisor.tick(10);

    TEST_ASSERT_EQUAL_UINT32(1, rig.store.size());        // serialized and enqueued,
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));          // the hunt never paused (stop() not called),
    TEST_ASSERT_EQUAL_INT(0, rig.station.bringUpCalls);   // no associate,
    TEST_ASSERT_EQUAL_UINT32(0, rig.uploader.calls.size());  // and no upload.
}

void test_threshold_triggers_one_batched_drain_that_uploads_all_and_resumes(void) {
    UploadSupervisorConfig config;
    config.drainThreshold = 3;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);

    for (uint8_t i = 0; i < 3; ++i) rig.supervisor.onCaptureReady(makeHandshake(bssidN(i).data()));
    TEST_ASSERT_EQUAL_UINT32(3, rig.store.size());

    rig.supervisor.tick(100);  // threshold reached -> stop the engine, enter the settle.
    TEST_ASSERT_TRUE(isIdle(rig.engine));  // the hunt is paused for the drain.

    rig.supervisor.tick(106);  // settle (5ms) elapsed -> one drain cycle runs to completion.
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);      // a single associate for the whole batch,
    TEST_ASSERT_EQUAL_INT(1, rig.station.tearDownCalls);
    TEST_ASSERT_EQUAL_UINT32(3, rig.uploader.calls.size());  // each capture uploaded exactly once,
    TEST_ASSERT_EQUAL_STRING(kKey, rig.uploader.calls[0].key.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());           // all deleted on the accepted result,
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));             // and the hunt resumed.
}

void test_a_rejected_upload_is_retried_never_dropped_and_backoff_grows(void) {
    UploadSupervisorConfig config;
    config.drainThreshold = 1;  // a single capture triggers a drain, so we can force two cycles.
    config.settleMs = 5;
    config.backoffBaseMs = 100;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    rig.uploader.scripted = {UploadResult::Rejected, UploadResult::Accepted};  // fail then succeed.
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));

    // First cycle: the upload is rejected, so the capture stays queued and is not deleted.
    rig.supervisor.tick(0);
    rig.supervisor.tick(6);  // past the settle -> cycle runs.
    TEST_ASSERT_EQUAL_UINT32(1, rig.uploader.calls.size());
    TEST_ASSERT_EQUAL_UINT32(1, rig.store.size());   // still pending — never silently dropped.
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));

    // The backoff now gates the retry: a tick well before it elapses runs no second cycle.
    rig.supervisor.tick(50);
    TEST_ASSERT_EQUAL_UINT32(1, rig.uploader.calls.size());  // backoff still holding.

    // Past the backoff (drain at 6 -> next allowed at 6+100=106): the retry runs and succeeds.
    rig.supervisor.tick(106);
    rig.supervisor.tick(112);  // past this cycle's settle.
    TEST_ASSERT_EQUAL_UINT32(2, rig.uploader.calls.size());  // retried,
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());           // and deleted only on the accepted result.
}

void test_a_duplicate_response_is_a_terminal_success(void) {
    UploadSupervisorConfig config;
    config.drainThreshold = 1;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    rig.uploader.defaultResult = UploadResult::Duplicate;  // wpa-sec already holds it.
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));
    rig.supervisor.tick(0);
    rig.supervisor.tick(6);

    TEST_ASSERT_EQUAL_UINT32(1, rig.uploader.calls.size());
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());  // deleted exactly as for accepted — off our hands.
    TEST_ASSERT_EQUAL(static_cast<int>(1), static_cast<int>(rig.supervisor.lastDrain().duplicate));
}

void test_backoff_grows_on_failure_and_resets_on_success(void) {
    UploadSupervisorConfig config;
    config.drainThreshold = 1;
    config.settleMs = 5;
    config.backoffBaseMs = 100;
    config.backoffCapMs = 100000;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    rig.station.bringUpSucceeds = false;  // offline: every cycle fails to associate.
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));

    // Cycle 1 at t=0 (settle to 5). Failure -> next allowed at 5+100=105, backoff doubles to 200.
    rig.supervisor.tick(0);
    rig.supervisor.tick(5);
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);

    // Backoff = 100ms: no cycle before t=105.
    rig.supervisor.tick(104);
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);
    rig.supervisor.tick(105);  // cycle 2 (settle to 110). Failure -> next allowed 110+200=310.
    rig.supervisor.tick(110);
    TEST_ASSERT_EQUAL_INT(2, rig.station.bringUpCalls);

    // Backoff has grown to 200ms: no cycle before t=310 (proving the interval doubled).
    rig.supervisor.tick(309);
    TEST_ASSERT_EQUAL_INT(2, rig.station.bringUpCalls);
    rig.supervisor.tick(310);  // cycle 3 (settle to 315). Failure -> next allowed 315+400=715.
    rig.supervisor.tick(315);
    TEST_ASSERT_EQUAL_INT(3, rig.station.bringUpCalls);

    // Now come back online: the next cycle succeeds and must reset the backoff to the base.
    rig.station.bringUpSucceeds = true;
    rig.supervisor.tick(715);  // cycle 4 (settle to 720). Success -> backoff resets, queue drains.
    rig.supervisor.tick(720);
    TEST_ASSERT_EQUAL_INT(4, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());

    // Prove the reset: a fresh capture that fails is gated by the base (100ms) again, not the grown
    // interval. Fail once more, then check the gate is at now+base.
    rig.station.bringUpSucceeds = false;
    rig.supervisor.onCaptureReady(makeHandshake(bssidN(2).data()));
    rig.supervisor.tick(800);  // cycle 5 (settle to 805). Failure -> next allowed 805+100=905.
    rig.supervisor.tick(805);
    TEST_ASSERT_EQUAL_INT(5, rig.station.bringUpCalls);
    rig.supervisor.tick(904);
    TEST_ASSERT_EQUAL_INT(5, rig.station.bringUpCalls);  // still within the base backoff,
    rig.supervisor.tick(905);
    rig.supervisor.tick(910);
    TEST_ASSERT_EQUAL_INT(6, rig.station.bringUpCalls);  // base interval, not the grown one -> reset.
}

void test_an_unreadable_capture_is_purged_so_the_cycle_stays_clean(void) {
    // Regression for the backoff wedge: a permanently-unreadable (corrupt) entry must be purged, not
    // kept, so the cycle can reach "clean" and backoff recovers — otherwise one bad file pins the drain
    // cadence at the backoff cap forever (ADR-0017 decision #7). The good capture in the same batch must
    // still upload and delete as normal.
    UploadSupervisorConfig config;
    config.drainThreshold = 2;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));  // becomes id 1 (the corrupt one),
    rig.supervisor.onCaptureReady(makeHandshake(bssidN(2).data()));  // becomes id 2 (uploads fine).
    TEST_ASSERT_EQUAL_UINT32(2, rig.store.size());
    rig.store.failReadId = rig.store.entries()[0].id;  // the oldest entry is unreadable.

    rig.supervisor.tick(100);  // threshold reached -> stop, settle.
    rig.supervisor.tick(106);  // settle elapsed -> one drain cycle.

    TEST_ASSERT_EQUAL_UINT32(1, rig.supervisor.lastDrain().purged);       // the corrupt entry purged,
    TEST_ASSERT_EQUAL_UINT32(1, rig.supervisor.lastDrain().accepted);     // the good one uploaded,
    TEST_ASSERT_EQUAL_UINT32(0, rig.supervisor.lastDrain().storeErrors);  // purge is not a store error,
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());                        // both gone from the queue,
    TEST_ASSERT_EQUAL_UINT32(0, rig.supervisor.pendingActivity());        // and the cycle counted CLEAN
                                                                         // (activity reset -> no wedge).
}

void test_a_failed_boot_seed_does_not_strand_prior_captures(void) {
    // Regression: if the begin() seed listing fails, the supervisor must not assume the queue is empty.
    // Before the fix it left activity_ at 0, and shouldDrain()'s activity_==0 short-circuit suppressed
    // even the time ceiling, so a capture already on flash from a prior run was stranded until some new
    // capture happened to arrive. A warm trigger must let the time ceiling drain it.
    UploadSupervisorConfig config;
    config.drainThreshold = 8;          // high, so only the time ceiling can trigger here.
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000;
    Rig rig(config);
    rig.uploader.defaultResult = UploadResult::Accepted;

    TEST_ASSERT_TRUE(rig.engine.begin(0));
    // A capture the prior run left on flash (persisted directly through the queue, no supervisor state).
    TEST_ASSERT_EQUAL(static_cast<int>(OfferResult::Stored),
                      static_cast<int>(rig.queue.offer(makeHandshake(bssidN(1).data()))));
    TEST_ASSERT_EQUAL_UINT32(1, rig.store.size());

    rig.store.failList = true;   // the boot seed cannot read the queue,
    rig.supervisor.begin(0);
    rig.store.failList = false;  // listing recovers for the actual drain.

    // No new capture ever arrives. Only the time ceiling can act, and it must.
    rig.supervisor.tick(1000);   // ceiling reached -> stop, settle,
    rig.supervisor.tick(1006);   // settle elapsed -> drain runs.

    TEST_ASSERT_EQUAL_UINT32(1, rig.uploader.calls.size());  // the stranded capture was drained,
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());           // and cleared.
}

// --- Window sharing with the cracked-results sync (slice-0020 Scenario H, integration half) ---------

void test_a_due_sync_piggybacks_the_drain_window_with_one_associate(void) {
    // A drain triggered by the upload threshold also runs a due sync inside the SAME STA window — one
    // associate for the whole batch and the sync, never a second concurrent STA path (ADR-0019 #6).
    UploadSupervisorConfig config;
    config.drainThreshold = 3;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    FakeSyncSession sync;
    sync.dueFlag = true;
    Rig rig(config, &sync);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);
    TEST_ASSERT_EQUAL_INT(1, sync.beginCalls);  // the cadence was seeded exactly once, at begin().

    for (uint8_t i = 0; i < 3; ++i) rig.supervisor.onCaptureReady(makeHandshake(bssidN(i).data()));
    rig.supervisor.tick(100);  // threshold reached -> stop, settle.
    rig.supervisor.tick(106);  // settle elapsed -> one cycle: drain + sync.

    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);       // a single associate,
    TEST_ASSERT_EQUAL_UINT32(3, rig.uploader.calls.size());   // all three uploaded,
    TEST_ASSERT_EQUAL_INT(1, sync.runCalls);                  // and the sync ran once in that window,
    TEST_ASSERT_EQUAL_UINT32(0, rig.store.size());
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));              // and the hunt resumed.
}

void test_a_due_sync_forces_one_window_with_an_empty_queue_rate_limited(void) {
    // The queue stays empty but the hour comes due: the supervisor opens ONE window for the sync alone,
    // promptly, then paces retries by syncRetryIntervalMs (not every tick) — ADR-0019 decision #6.
    UploadSupervisorConfig config;
    config.drainThreshold = 8;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;  // upload trickle ceiling well out of the way.
    config.syncRetryIntervalMs = 5000;    // the sync-only window's own retry cadence.
    FakeSyncSession sync;
    sync.dueFlag = true;  // due and latched (models a sync owed until it succeeds).
    Rig rig(config, &sync);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);  // empty queue -> activity_ == 0; the sync window is armed to open at once.

    rig.supervisor.tick(10);  // due sync -> a window opens promptly (not waiting the upload ceiling).
    rig.supervisor.tick(20);  // cycle: associate, empty drain, sync runs, teardown.
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_INT(1, sync.runCalls);
    TEST_ASSERT_EQUAL_UINT32(0, rig.uploader.calls.size());  // nothing uploaded — a sync-only window,
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));

    rig.supervisor.tick(21);  // immediately after: still due, but the retry interval gates it -> no thrash.
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);

    rig.supervisor.tick(5020);  // past the retry interval (ran at 20 -> next at 20+5000) -> retry.
    rig.supervisor.tick(5030);
    TEST_ASSERT_EQUAL_INT(2, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_INT(2, sync.runCalls);
}

void test_a_due_sync_opens_a_window_independent_of_a_long_upload_ceiling(void) {
    // Regression (adversarial finding): the forced sync-only window must follow the sync cadence, not the
    // upload trickle ceiling. With maxDrainIntervalMs raised far above an hour (a legitimate low-activity
    // tuning), a due sync must still open its window promptly (ADR-0019 decision #6) — before the fix it
    // waited for that ceiling and the hourly sync silently degraded to the upload interval.
    UploadSupervisorConfig config;
    config.drainThreshold = 8;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 7200000;  // 2h — longer than the 1h sync interval.
    FakeSyncSession sync;
    sync.dueFlag = true;
    Rig rig(config, &sync);
    rig.start(0);

    rig.supervisor.tick(1000);  // far below the 2h ceiling, but a sync is due -> a window must open.
    rig.supervisor.tick(1010);  // past the settle -> the cycle runs the sync.
    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_INT(1, sync.runCalls);
}

void test_a_sync_runs_only_inside_a_live_window_and_a_failed_associate_fakes_no_activity(void) {
    // Offline: a forced sync window that cannot associate must NOT run the sync (it needs a live STA
    // session), and must NOT invent upload activity — the old defensive `activity_=1` would open a
    // needless empty drain later; the sync's own due() drives its retry instead.
    UploadSupervisorConfig config;
    config.drainThreshold = 8;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000;
    config.backoffBaseMs = 100;
    FakeSyncSession sync;
    sync.dueFlag = true;
    Rig rig(config, &sync);
    rig.station.bringUpSucceeds = false;  // offline.
    rig.start(0);

    rig.supervisor.tick(1000);  // ceiling -> forced sync window: stop, settle.
    rig.supervisor.tick(1006);  // cycle: bring-up fails.

    TEST_ASSERT_EQUAL_INT(1, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_INT(0, sync.runCalls);                    // never run without a live window,
    TEST_ASSERT_EQUAL_UINT32(0, rig.supervisor.pendingActivity());  // and no faked upload activity.
    TEST_ASSERT_TRUE(isDiscovering(rig.engine));
}

void test_a_sync_not_due_never_forces_a_window(void) {
    // An idle appliance with an empty queue and no sync owed never associates — the forced window is
    // strictly sync-gated, so it cannot become a spurious periodic associate.
    UploadSupervisorConfig config;
    config.drainThreshold = 8;
    config.maxDrainIntervalMs = 1000;
    FakeSyncSession sync;
    sync.dueFlag = false;  // nothing owed.
    Rig rig(config, &sync);
    rig.start(0);

    rig.supervisor.tick(1000);  // past the ceiling, but nothing to send and nothing due.
    rig.supervisor.tick(5000);
    TEST_ASSERT_EQUAL_INT(0, rig.station.bringUpCalls);
    TEST_ASSERT_EQUAL_INT(0, sync.runCalls);
}

void test_a_drain_cycle_publishes_start_and_completion_facts_on_the_bus(void) {
    // The supervisor is a bus producer (ADR-0021): opening a window publishes DrainStarted, and each
    // completed cycle publishes DrainCompleted carrying the same outcome lastDrain() exposes. Proven so
    // a surface (the LED) can be sure it sees both edges of every window.
    UploadSupervisorConfig config;
    config.drainThreshold = 1;  // one capture opens a window.
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    EventBus bus;
    sapper_test::RecordingEventSink sink;
    TEST_ASSERT_TRUE(bus.subscribe(sink));
    Rig rig(config, nullptr, &bus);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));
    rig.supervisor.tick(100);  // window opens -> DrainStarted; engine stops for the settle.
    TEST_ASSERT_EQUAL_INT(1, sink.drainStarted);
    TEST_ASSERT_EQUAL_size_t(0, sink.drainsCompleted.size());  // not completed yet — only started.

    rig.supervisor.tick(106);  // settle elapsed -> the cycle runs to completion -> DrainCompleted.
    TEST_ASSERT_EQUAL_INT(1, sink.drainStarted);
    TEST_ASSERT_EQUAL_size_t(1, sink.drainsCompleted.size());
    TEST_ASSERT_TRUE(sink.drainsCompleted[0].associated);
    TEST_ASSERT_EQUAL_size_t(1, sink.drainsCompleted[0].accepted);  // the same counted outcome as the snapshot.
}

// --- Capture-notification fact (slice-0032 Scenarios A, B, C; ADR-0031) ----------------------------

/// A beacon-only "handshake": no EAPOL, so serializeHandshake() rejects it and offer() returns
/// NotUploadable — the shape a well-behaved engine never reports, re-checked here (fail loud).
static CapturedHandshake makeBeaconOnly(const uint8_t bssid[6]) {
    HandshakeCollector collector(bssid, 6);
    const std::vector<uint8_t> beacon = buildBeacon(bssid, "Net", 6);
    collector.ingest(beacon.data(), static_cast<uint16_t>(beacon.size()));
    return collector.handshake();
}

void test_a_successful_enqueue_publishes_handshake_captured_with_identity(void) {
    // The gap this slice closes (ADR-0031): a captured handshake must raise a local fact, not just tick
    // the upload counter. A successful enqueue publishes exactly one HandshakeCaptured carrying the
    // network's identity — and only its identity (§4 #9/#17: CaptureFact has no frame bytes). Fail-before:
    // without the publishCaptured() call, captures stays empty.
    UploadSupervisorConfig config;
    config.drainThreshold = 100;  // stay in Hunting; we assert the enqueue-time fact, not a drain.
    EventBus bus;
    sapper_test::RecordingEventSink sink;
    TEST_ASSERT_TRUE(bus.subscribe(sink));
    Rig rig(config, nullptr, &bus);
    rig.start(0);

    const auto bssid = bssidN(7);
    rig.supervisor.onCaptureReady(makeHandshake(bssid.data()));

    TEST_ASSERT_EQUAL_size_t(1, sink.captures.size());
    TEST_ASSERT_EQUAL_MEMORY(bssid.data(), sink.captures[0].bssid, 6);
    TEST_ASSERT_EQUAL_STRING("Net", sink.captures[0].ssid);  // the beacon's SSID, carried for the banner.
}

void test_a_hidden_network_capture_carries_an_empty_ssid(void) {
    // A hidden AP beacons no SSID, so the fact carries an empty ssid (the banner falls back to BSSID).
    UploadSupervisorConfig config;
    config.drainThreshold = 100;
    EventBus bus;
    sapper_test::RecordingEventSink sink;
    TEST_ASSERT_TRUE(bus.subscribe(sink));
    Rig rig(config, nullptr, &bus);
    rig.start(0);

    const auto bssid = bssidN(8);
    HandshakeCollector collector(bssid.data(), 6);
    const std::vector<uint8_t> beacon = buildBeacon(bssid.data(), "", 6);  // hidden: empty SSID.
    collector.ingest(beacon.data(), static_cast<uint16_t>(beacon.size()));
    const std::vector<uint8_t> m1 = buildEapol(bssid.data(), kClient, sapper_test::kKeyInfoM1, true);
    collector.ingest(m1.data(), static_cast<uint16_t>(m1.size()));
    const std::vector<uint8_t> m2 = buildEapol(bssid.data(), kClient, sapper_test::kKeyInfoM2, false);
    collector.ingest(m2.data(), static_cast<uint16_t>(m2.size()));
    rig.supervisor.onCaptureReady(collector.handshake());

    TEST_ASSERT_EQUAL_size_t(1, sink.captures.size());
    TEST_ASSERT_EQUAL_STRING("", sink.captures[0].ssid);
}

void test_a_failed_enqueue_publishes_no_capture_fact(void) {
    // The fact must never announce a capture the queue actually dropped (ADR-0031 #1): it is published
    // AFTER a successful offer, only on the success arms. A store failure and an unuploadable handshake
    // both publish nothing.
    UploadSupervisorConfig config;
    config.drainThreshold = 100;
    EventBus bus;
    sapper_test::RecordingEventSink sink;
    TEST_ASSERT_TRUE(bus.subscribe(sink));
    Rig rig(config, nullptr, &bus);
    rig.start(0);

    rig.store.failEnqueue = true;  // offer() -> StoreError.
    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));
    TEST_ASSERT_EQUAL_size_t(0, sink.captures.size());

    rig.store.failEnqueue = false;
    rig.supervisor.onCaptureReady(makeBeaconOnly(bssidN(2).data()));  // offer() -> NotUploadable.
    TEST_ASSERT_EQUAL_size_t(0, sink.captures.size());
}

// --- Window sharing with a transmitting surface (slice-0024 Scenario J; ADR-0023) ------------------

void test_a_live_window_flushes_the_notifier_and_an_offline_cycle_does_not(void) {
    // The webhook transmits inside the STA window: the supervisor must flush its WindowNotifier once per
    // live cycle (so a fresh crack is pushed that window), and must NOT flush an offline cycle (there is
    // no association to POST over). Fail-before: without the runDrainCycle flush call, flushCalls stays 0.
    using sapper_test::FakeWindowNotifier;
    UploadSupervisorConfig config;
    config.drainThreshold = 1;
    config.settleMs = 5;
    config.maxDrainIntervalMs = 1000000;
    Rig rig(config);
    FakeWindowNotifier notifier;
    rig.supervisor.setNotifier(notifier);
    rig.uploader.defaultResult = UploadResult::Accepted;
    rig.start(0);

    rig.supervisor.onCaptureReady(makeHandshake(bssidN(1).data()));
    rig.supervisor.tick(100);  // window opens -> stop, settle.
    rig.supervisor.tick(106);  // settle elapsed -> one live cycle: drain + flush inside the window.
    TEST_ASSERT_EQUAL_INT(1, notifier.flushCalls);       // flushed once, in the live window,
    TEST_ASSERT_EQUAL_INT(1, rig.station.tearDownCalls);  // which was then torn down.

    // An offline cycle: the associate fails, so there is no window and the notifier is not flushed.
    rig.station.bringUpSucceeds = false;
    rig.supervisor.onCaptureReady(makeHandshake(bssidN(2).data()));
    rig.supervisor.tick(200);
    rig.supervisor.tick(206);
    TEST_ASSERT_EQUAL_INT(2, rig.station.bringUpCalls);  // a second cycle was attempted,
    TEST_ASSERT_EQUAL_INT(1, notifier.flushCalls);       // but no flush — it never associated.
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_capture_below_threshold_is_enqueued_without_pausing_the_hunt);
    RUN_TEST(test_threshold_triggers_one_batched_drain_that_uploads_all_and_resumes);
    RUN_TEST(test_a_rejected_upload_is_retried_never_dropped_and_backoff_grows);
    RUN_TEST(test_a_duplicate_response_is_a_terminal_success);
    RUN_TEST(test_backoff_grows_on_failure_and_resets_on_success);
    RUN_TEST(test_an_unreadable_capture_is_purged_so_the_cycle_stays_clean);
    RUN_TEST(test_a_failed_boot_seed_does_not_strand_prior_captures);
    RUN_TEST(test_a_due_sync_piggybacks_the_drain_window_with_one_associate);
    RUN_TEST(test_a_due_sync_forces_one_window_with_an_empty_queue_rate_limited);
    RUN_TEST(test_a_due_sync_opens_a_window_independent_of_a_long_upload_ceiling);
    RUN_TEST(test_a_sync_runs_only_inside_a_live_window_and_a_failed_associate_fakes_no_activity);
    RUN_TEST(test_a_sync_not_due_never_forces_a_window);
    RUN_TEST(test_a_drain_cycle_publishes_start_and_completion_facts_on_the_bus);
    RUN_TEST(test_a_successful_enqueue_publishes_handshake_captured_with_identity);
    RUN_TEST(test_a_hidden_network_capture_carries_an_empty_ssid);
    RUN_TEST(test_a_failed_enqueue_publishes_no_capture_fact);
    RUN_TEST(test_a_live_window_flushes_the_notifier_and_an_offline_cycle_does_not);
    return UNITY_END();
}
