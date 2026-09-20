/**
 * @file test_hunt_engine.cpp
 * @brief Native unit tests for the endless-AutoHunt state machine (slice-0016 Scenarios A-D, ADR-0015).
 *
 * The whole loop is driven through an injected fake RadioSniffer and a fake clock — no radio. Frames
 * enter through the fake sniffer's deliver(), which calls the exact onFrame() the device callback
 * calls (§4 invariant #2), so this proves the orchestration reaches the pure sinks through the real
 * seam. It covers the discover→capture→advance loop, the full-enumeration round-robin, the
 * retune-failure skip, and the quiesce settle that gates every sink clear.
 */
#include <unity.h>

#include <array>
#include <cstring>
#include <vector>

#include "net/ap_registry.h"
#include "net/deauth.h"
#include "net/hunt_engine.h"

#include "../support/fake_radio_sniffer.h"
#include "../support/fake_raw_transmitter.h"
#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;
using sapper_test::FakeRadioSniffer;
using sapper_test::FakeRawTransmitter;

static const uint8_t kApA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
static const uint8_t kApB[6] = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
static const uint8_t kApC[6] = {0x02, 0x01, 0x02, 0x03, 0x04, 0x05};
static const uint8_t kClient[6] = {0x06, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
static uint8_t kHopChannels[3] = {1, 6, 11};

// Short windows keep the deterministic tick sequence readable; the logic is clock-driven, not
// wall-clock, so the absolute values only need to be internally consistent.
static HuntConfig testConfig() {
    HuntConfig c;
    c.dwellMs = 10;
    c.discoverWindowMs = 30;
    c.captureWindowMs = 30;
    c.settleMs = 5;
    c.deauthIntervalMs = 10;  // short cadence so a burst-then-gap is visible within a capture window.
    return c;
}

struct RecordingObserver : public CaptureReadyObserver {
    std::vector<std::array<uint8_t, 6>> captured;
    void onCaptureReady(const CapturedHandshake& handshake) override {
        TEST_ASSERT_TRUE(handshake.isWpaSecValid());  // the engine only reports valid captures.
        std::array<uint8_t, 6> bssid{};
        std::memcpy(bssid.data(), handshake.bssid, 6);
        captured.push_back(bssid);
    }
};

void setUp(void) {}
void tearDown(void) {}

static bool bssidIs(const uint8_t* got, const uint8_t* want) {
    return got != nullptr && std::memcmp(got, want, 6) == 0;
}

/// Deliver a full wpa-sec-valid handshake (beacon + M1 + M2) for @p bssid into the sniffer.
static void deliverHandshake(FakeRadioSniffer& sniffer, const uint8_t bssid[6], int channel) {
    sniffer.deliver(buildBeacon(bssid, "Net", channel));
    sniffer.deliver(buildEapol(bssid, kClient, sapper_test::kKeyInfoM1, true));
    sniffer.deliver(buildEapol(bssid, kClient, sapper_test::kKeyInfoM2, false));
}

void test_discovers_then_captures_then_advances(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());

    TEST_ASSERT_TRUE(engine.begin(0));
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));

    // Two distinct APs beacon during the discovery window (each on its own channel).
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(0);
    engine.tick(10);
    sniffer.deliver(buildBeacon(kApB, "Bravo", 6));
    engine.tick(20);
    TEST_ASSERT_EQUAL_UINT32(2, engine.discoveredCount());

    engine.tick(30);  // discovery window ends -> quiesce toward capture.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));
    engine.tick(35);  // settle elapsed -> capture the first target (ApA, channel 1).
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
    TEST_ASSERT_TRUE(bssidIs(engine.capturingBssid(), kApA));
    TEST_ASSERT_EQUAL_UINT8(1, sniffer.currentChannel);

    deliverHandshake(sniffer, kApA, 1);
    engine.tick(36);  // wpa-sec valid -> quiesce toward report+advance.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));
    engine.tick(41);  // settle elapsed -> fire event, advance to ApB.

    TEST_ASSERT_EQUAL_UINT32(1, observer.captured.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, observer.captured[0].data(), 6);
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
    TEST_ASSERT_TRUE(bssidIs(engine.capturingBssid(), kApB));
    TEST_ASSERT_EQUAL_UINT8(6, sniffer.currentChannel);
}

void test_round_robin_visits_every_ap_then_rediscovers(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());

    TEST_ASSERT_TRUE(engine.begin(0));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    sniffer.deliver(buildBeacon(kApB, "Bravo", 6));
    sniffer.deliver(buildBeacon(kApC, "Charlie", 11));
    engine.tick(30);  // snapshot 3 targets -> quiesce.

    // Walk each capture window to its timeout (no handshake arrives); record who is captured, in order.
    std::vector<std::array<uint8_t, 6>> order;
    uint32_t now = 35;
    for (int i = 0; i < 3; ++i) {
        engine.tick(now);  // settle elapsed -> start capturing target i.
        TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
        std::array<uint8_t, 6> b{};
        std::memcpy(b.data(), engine.capturingBssid(), 6);
        order.push_back(b);
        now += testConfig().captureWindowMs;
        engine.tick(now);        // capture window times out -> quiesce toward advance.
        now += testConfig().settleMs;
    }
    engine.tick(now);  // after the third target's advance settle -> back to discovery.

    TEST_ASSERT_EQUAL_UINT32(3, order.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, order[0].data(), 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApB, order[1].data(), 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApC, order[2].data(), 6);
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));
    TEST_ASSERT_EQUAL_UINT32(0, engine.discoveredCount());  // the registry was reset for the new sweep.
    TEST_ASSERT_EQUAL_UINT32(0, observer.captured.size());  // no handshake ever arrived.
}

void test_target_with_rejected_channel_is_skipped(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());

    TEST_ASSERT_TRUE(engine.begin(0));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));   // first target, channel 1.
    sniffer.deliver(buildBeacon(kApB, "Bravo", 6));   // second target, channel 6.
    sniffer.rejectedChannels.insert(1);               // the radio cannot tune to ApA's channel.
    engine.tick(30);  // snapshot -> quiesce toward capture.

    engine.tick(35);  // settle elapsed -> ApA's channel is rejected, so skip to ApB.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
    TEST_ASSERT_TRUE(bssidIs(engine.capturingBssid(), kApB));  // never captured ApA off-channel.
    TEST_ASSERT_EQUAL_UINT8(6, sniffer.currentChannel);

    // A handshake for the skipped ApA (were it somehow heard) must never be reported.
    deliverHandshake(sniffer, kApB, 6);
    engine.tick(36);
    engine.tick(41);
    TEST_ASSERT_EQUAL_UINT32(1, observer.captured.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApB, observer.captured[0].data(), 6);
}

void test_sink_is_cleared_only_after_the_quiesce_settle(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());

    TEST_ASSERT_TRUE(engine.begin(0));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));  // one AP discovered.
    engine.tick(30);  // snapshot 1 target.
    engine.tick(35);  // capture ApA.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
    engine.tick(65);  // capture window (35+30) times out -> quiesce toward re-discovery.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));

    // Mid-quiesce, before the settle (65 + 5 = 70) elapses: the registry still holds its entry and a
    // delivered frame reaches no sink (the router is aimed away), so it is neither recorded nor lost
    // into a half-reset table.
    TEST_ASSERT_EQUAL_UINT32(1, engine.discoveredCount());
    sniffer.deliver(buildBeacon(kApB, "Bravo", 6));
    engine.tick(66);
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));
    TEST_ASSERT_EQUAL_UINT32(1, engine.discoveredCount());  // not reset yet, and ApB was dropped.

    engine.tick(70);  // settle elapsed -> registry reset, new sweep begins.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));
    TEST_ASSERT_EQUAL_UINT32(0, engine.discoveredCount());
}

void test_begin_fails_when_sniffer_cannot_start(void) {
    FakeRadioSniffer sniffer;
    sniffer.beginSucceeds = false;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());

    TEST_ASSERT_FALSE(engine.begin(0));
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Idle), static_cast<int>(engine.phase()));
    engine.tick(10);  // ticking an Idle engine is a no-op, never a crash.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Idle), static_cast<int>(engine.phase()));
}

void test_discovery_window_is_wraparound_safe(void) {
    // The appliance runs endlessly, so millis() wraps (~49.7 days) as normal operation. A discovery
    // window whose deadline (now + window) straddles the wrap must not be cut short (ADR-0015 / the
    // ChannelHopper wrap discipline, ADR-0013). Begin so the 4000ms window's deadline wraps to a small
    // value while `now` is still large.
    HuntConfig config = testConfig();
    config.discoverWindowMs = 4000;
    config.dwellMs = 1000000;  // huge: keep the hopper from advancing during this test.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, config);

    const uint32_t base = 0xFFFFFF00u;  // deadline = base + 4000 wraps to 0xEA0.
    TEST_ASSERT_TRUE(engine.begin(base));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(base + 10);  // only 10ms into a 4000ms window — a direct `now >= deadline` fires here.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));

    engine.tick(static_cast<uint32_t>(base + 4000));  // true window elapsed, across the wrap.
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));
}

void test_quiesce_settle_is_wraparound_safe(void) {
    // The settle is the concurrency-critical deadline: if it collapses to zero at the millis() wrap, a
    // sink is reset while a driver-task onFrame may still be writing it (§4 invariant #11). Enter the
    // quiesce within settleMs of the wrap so quiesceUntil (now + 5) wraps past 0xFFFFFFFF.
    HuntConfig config = testConfig();
    config.discoverWindowMs = 10;
    config.settleMs = 5;
    config.dwellMs = 1000000;
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, config);

    const uint32_t base = 0xFFFFFFF4u;  // discover deadline = base + 10 = 0xFFFFFFFE.
    TEST_ASSERT_TRUE(engine.begin(base));
    engine.tick(0xFFFFFFFEu);  // window elapsed (0 APs) -> enterQuiesce; quiesceUntil = 0xFFFFFFFE+5 wraps to 0x3.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));

    engine.tick(0xFFFFFFFFu);  // only 1ms into a 5ms settle — a direct `now < until` would exit early.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));
    engine.tick(0x2u);  // 4ms elapsed across the wrap — still short of 5ms.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Quiescing), static_cast<int>(engine.phase()));

    engine.tick(0x3u);  // true 5ms elapsed -> settle done, new sweep begins.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Discovering), static_cast<int>(engine.phase()));
}

// --- slice-0030: autonomous in-loop deauth, armed by an injected transmitter (ADR-0029) -----------

// Drive an armed engine to the point of capturing kApA on channel 1, then one Capturing tick so the
// first deauth burst has fired. Returns with phase == Capturing. Mirrors the discover→capture walk
// the other tests use, so the deauth behaviour is exercised through the real state machine, not a
// shortcut.
static void driveToCapturingWithFirstBurst(HuntEngine& engine, FakeRadioSniffer& sniffer) {
    TEST_ASSERT_TRUE(engine.begin(0));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(30);  // discovery window ends -> quiesce toward capture.
    engine.tick(35);  // settle elapsed -> capture kApA on channel 1 (arms the deauth cadence at t=35).
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));
    engine.tick(36);  // first Capturing tick: cadence due -> one deauth + disassoc burst.
}

void test_armed_engine_deauths_current_target_broadcast_during_capturing(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    FakeRawTransmitter tx;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig(), &tx);

    driveToCapturingWithFirstBurst(engine, sniffer);

    // One burst = a deauth AND a disassoc frame, both broadcast at the AP being captured.
    TEST_ASSERT_EQUAL_UINT32(2, tx.count());
    TEST_ASSERT_EQUAL_UINT32(2, engine.deauthTxOk());
    TEST_ASSERT_EQUAL_UINT32(0, engine.deauthTxFail());

    const std::vector<uint8_t>& deauth = tx.frames[0];
    TEST_ASSERT_EQUAL_UINT32(kDeauthFrameLen, deauth.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ManagementSubtype::Deauth), deauth[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBroadcastMac, &deauth[4], 6);   // Addr1 destination = broadcast.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, &deauth[10], 6);           // Addr2 source = the target BSSID.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, &deauth[16], 6);           // Addr3 BSSID = the target BSSID.
}

void test_each_burst_is_a_deauth_and_a_disassoc(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    FakeRawTransmitter tx;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig(), &tx);

    driveToCapturingWithFirstBurst(engine, sniffer);

    TEST_ASSERT_EQUAL_UINT32(2, tx.count());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ManagementSubtype::Deauth), tx.frames[0][0]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ManagementSubtype::Disassoc), tx.frames[1][0]);
    // The disassoc is broadcast at the same target — differs from the deauth only in the subtype.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kBroadcastMac, &tx.frames[1][4], 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, &tx.frames[1][10], 6);
}

void test_deauth_respects_the_cadence_not_one_per_tick(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    FakeRawTransmitter tx;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig(), &tx);

    driveToCapturingWithFirstBurst(engine, sniffer);  // first burst at t=36; next due at 36+10=46.
    TEST_ASSERT_EQUAL_UINT32(2, tx.count());

    engine.tick(40);  // within the cadence gap (<46): no new burst — the listen gap for reassociation.
    TEST_ASSERT_EQUAL_UINT32(2, tx.count());
    engine.tick(46);  // cadence elapsed: exactly one more burst (two frames), not one per tick.
    TEST_ASSERT_EQUAL_UINT32(4, tx.count());
}

void test_disarmed_engine_never_transmits(void) {
    // The default-off safety gate (invariant #16): a nullptr transmitter (the ctor default) is a
    // purely passive hunt. This is the property a lost or unarmed shipped unit depends on.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());  // no transmitter.

    driveToCapturingWithFirstBurst(engine, sniffer);
    // Run the rest of the capture window too, so a full Capturing phase is exercised.
    engine.tick(46);
    engine.tick(56);

    TEST_ASSERT_EQUAL_UINT32(0, engine.deauthTxOk());
    TEST_ASSERT_EQUAL_UINT32(0, engine.deauthTxFail());
}

void test_rejected_frames_count_as_txfail_not_txok(void) {
    // The radio rejecting a raw frame (bypass inactive, interface down) must land in deauthTxFail, not
    // txOk — the on-air verify's heartbeat reads both, and a swapped/dropped fail branch would let a
    // device that transmits nothing report success. FakeRawTransmitter.nextResult=false is that reject.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    FakeRawTransmitter tx;
    tx.nextResult = false;  // the radio refuses every frame.
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig(), &tx);

    driveToCapturingWithFirstBurst(engine, sniffer);  // one burst attempted = deauth + disassoc.

    TEST_ASSERT_EQUAL_UINT32(2, tx.count());          // both frames were handed to the seam,
    TEST_ASSERT_EQUAL_UINT32(0, engine.deauthTxOk()); // but neither counted as a success,
    TEST_ASSERT_EQUAL_UINT32(2, engine.deauthTxFail()); // both counted as failures.
}

void test_deauth_only_while_capturing_and_stops_once_captured(void) {
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    FakeRawTransmitter tx;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig(), &tx);

    TEST_ASSERT_TRUE(engine.begin(0));
    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(10);  // Discovering — the radio is not yet on a target.
    engine.tick(20);
    TEST_ASSERT_EQUAL_UINT32(0, engine.deauthTxOk());  // no deauth outside Capturing.

    engine.tick(30);  // -> quiesce toward capture.
    engine.tick(35);  // -> Capturing kApA.
    engine.tick(36);  // first burst.
    TEST_ASSERT_GREATER_THAN_UINT32(0, engine.deauthTxOk());

    // The handshake arrives: the engine must stop deauthing the instant the capture succeeds.
    deliverHandshake(sniffer, kApA, 1);
    engine.tick(37);  // isWpaSecValid -> quiesce (ReportThenAdvance), no deauth.
    const uint32_t okAtCapture = engine.deauthTxOk();
    engine.tick(42);  // settle -> report + advance; one AP, so back to Discovering.
    engine.tick(43);  // Discovering again — no further deauth.
    engine.tick(44);
    TEST_ASSERT_EQUAL_UINT32(okAtCapture, engine.deauthTxOk());
    TEST_ASSERT_EQUAL_UINT32(1, observer.captured.size());
}

// --- Live-hunt pull snapshot (slice-0034 Scenarios A, B; ADR-0033) ---------------------------------

void test_snapshot_reflects_discovering(void) {
    // While Discovering, the snapshot reports the sweep: phase, the parked channel, and the AP count the
    // HUD's SCAN line shows. Fail-before: without huntSnapshot() this does not compile/return.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());
    TEST_ASSERT_TRUE(engine.begin(0));

    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(10);
    sniffer.deliver(buildBeacon(kApB, "Bravo", 6));
    engine.tick(20);

    const HuntSnapshot snap = engine.huntSnapshot();
    TEST_ASSERT_EQUAL(static_cast<int>(HuntPhase::Discovering), static_cast<int>(snap.phase));
    TEST_ASSERT_EQUAL_UINT32(2, snap.discovered);
    TEST_ASSERT_EQUAL_UINT8(engine.parkedChannel(), snap.channel);
}

void test_snapshot_reflects_populated_capturing(void) {
    // While Capturing, the snapshot names the target and reports which handshake pieces the collector
    // holds — the HUD's indicator row. Read before the wpa-sec-valid tick so the phase is still Capturing.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());
    TEST_ASSERT_TRUE(engine.begin(0));

    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(30);  // discovery ends -> quiesce.
    engine.tick(35);  // settle elapsed -> Capturing kApA.
    TEST_ASSERT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));

    deliverHandshake(sniffer, kApA, 1);  // beacon "Net" + M1 + M2 for the captured target.

    const HuntSnapshot snap = engine.huntSnapshot();
    TEST_ASSERT_EQUAL(static_cast<int>(HuntPhase::Capturing), static_cast<int>(snap.phase));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kApA, snap.bssid, 6);
    TEST_ASSERT_EQUAL_STRING("Net", snap.ssid);
    TEST_ASSERT_TRUE(snap.hasBeacon && snap.hasM1 && snap.hasM2);
    TEST_ASSERT_FALSE(snap.hasM3 || snap.hasM4);
    TEST_ASSERT_EQUAL_UINT8(3, snap.collectedCount());
}

void test_snapshot_hides_target_when_not_capturing(void) {
    // Once the engine leaves Capturing, the snapshot must NOT report a stale target — the phase guard
    // restricts the target/flag fields to the Capturing phase. Guards against dropping that guard: the
    // collector still physically holds the just-captured handshake here, so an unguarded read would leak it.
    FakeRadioSniffer sniffer;
    RecordingObserver observer;
    ApRegistry registry;
    HuntEngine engine(sniffer, registry, observer, kHopChannels, 3, testConfig());
    TEST_ASSERT_TRUE(engine.begin(0));

    sniffer.deliver(buildBeacon(kApA, "Alpha", 1));
    engine.tick(30);  // discovery ends -> quiesce.
    engine.tick(35);  // Capturing kApA.
    deliverHandshake(sniffer, kApA, 1);  // collector now holds kApA + "Net" + M1 + M2.
    engine.tick(36);  // wpa-sec valid -> enterQuiesce(ReportThenAdvance): phase leaves Capturing.
    TEST_ASSERT_NOT_EQUAL(static_cast<int>(HuntEngine::Phase::Capturing), static_cast<int>(engine.phase()));

    const HuntSnapshot snap = engine.huntSnapshot();
    const uint8_t zero[6] = {0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, snap.bssid, 6);        // no stale target,
    TEST_ASSERT_EQUAL_STRING("", snap.ssid);                    // no stale name,
    TEST_ASSERT_FALSE(snap.hasBeacon || snap.hasM1 || snap.hasM2 || snap.hasM3 || snap.hasM4);  // no stale flags.
    TEST_ASSERT_EQUAL_UINT8(0, snap.collectedCount());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_discovers_then_captures_then_advances);
    RUN_TEST(test_discovery_window_is_wraparound_safe);
    RUN_TEST(test_quiesce_settle_is_wraparound_safe);
    RUN_TEST(test_round_robin_visits_every_ap_then_rediscovers);
    RUN_TEST(test_target_with_rejected_channel_is_skipped);
    RUN_TEST(test_sink_is_cleared_only_after_the_quiesce_settle);
    RUN_TEST(test_begin_fails_when_sniffer_cannot_start);
    RUN_TEST(test_armed_engine_deauths_current_target_broadcast_during_capturing);
    RUN_TEST(test_each_burst_is_a_deauth_and_a_disassoc);
    RUN_TEST(test_deauth_respects_the_cadence_not_one_per_tick);
    RUN_TEST(test_disarmed_engine_never_transmits);
    RUN_TEST(test_rejected_frames_count_as_txfail_not_txok);
    RUN_TEST(test_deauth_only_while_capturing_and_stops_once_captured);
    RUN_TEST(test_snapshot_reflects_discovering);
    RUN_TEST(test_snapshot_reflects_populated_capturing);
    RUN_TEST(test_snapshot_hides_target_when_not_capturing);
    return UNITY_END();
}
