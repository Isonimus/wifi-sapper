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
#include "net/hunt_engine.h"

#include "../support/fake_radio_sniffer.h"
#include "../support/frame_builders.h"

using namespace sapper;
using sapper_test::buildBeacon;
using sapper_test::buildEapol;
using sapper_test::FakeRadioSniffer;

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

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_discovers_then_captures_then_advances);
    RUN_TEST(test_discovery_window_is_wraparound_safe);
    RUN_TEST(test_quiesce_settle_is_wraparound_safe);
    RUN_TEST(test_round_robin_visits_every_ap_then_rediscovers);
    RUN_TEST(test_target_with_rejected_channel_is_skipped);
    RUN_TEST(test_sink_is_cleared_only_after_the_quiesce_settle);
    RUN_TEST(test_begin_fails_when_sniffer_cannot_start);
    return UNITY_END();
}
