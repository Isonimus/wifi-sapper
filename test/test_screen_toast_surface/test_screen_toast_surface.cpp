/**
 * @file test_screen_toast_surface.cpp
 * @brief Native unit tests for the status-HUD/toast display policy (ADR-0025).
 *
 * The surface is pure and clock-driven, so all of it is asserted here against a fake renderer and a
 * fake clock: drain facts drive the HUD status the same way the LED reads them; the uploaded and cracks
 * counters accumulate from bus facts; the last sync line follows SyncCompleted; a NewPassword arms a
 * CRACKED banner carrying the ESSID + BSSID (never the PSK — there is no field for it) that clears after
 * a fixed hold; non-crack facts raise no banner; a liveness heartbeat toggles; and the surface renders
 * only when the view changes. Glyph legibility on the panel is the on-air verify's job (Scenario J,
 * lane 3); *what* to show is proven here.
 */
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "net/cracked_result.h"
#include "net/cracked_sync.h"        // SyncOutcome
#include "net/hunt_snapshot.h"       // HuntSnapshot / HuntPhase — the live-hunt pull (ADR-0033)
#include "net/upload_supervisor.h"   // DrainOutcome
#include "support/fake_hunt_snapshot_source.h"
#include "support/fake_screen_renderer.h"
#include "surface/screen_toast_surface.h"

using namespace sapper;
using sapper_test::FakeHuntSnapshotSource;
using sapper_test::FakeScreenRenderer;

void setUp(void) {}
void tearDown(void) {}

static const uint8_t kBssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/// Deliver an event then tick the clock, so a fact set in onAppEvent is rendered the same step (the
/// hunt loop ticks the surface right after the supervisor publishes — hunt_loop.cpp).
static void step(ScreenToastSurface& s, const AppEvent& e, uint32_t nowMs) {
    s.onAppEvent(e);
    s.tick(nowMs);
}

static DrainOutcome drain(bool associated, bool resumeFailed = false, size_t accepted = 0,
                          size_t duplicate = 0) {
    DrainOutcome o;
    o.ran = true;
    o.associated = associated;
    o.resumeFailed = resumeFailed;
    o.accepted = accepted;
    o.duplicate = duplicate;
    return o;
}

static AppEvent newPassword(CrackedResult& r, const char* essid, const char* psk) {
    std::snprintf(r.essid, sizeof(r.essid), "%s", essid);
    std::memcpy(r.bssid, kBssid, sizeof(r.bssid));
    std::snprintf(r.password, sizeof(r.password), "%s", psk);
    return AppEvent::newPassword(r);
}

static AppEvent captured(CaptureFact& f, const char* ssid) {
    std::snprintf(f.ssid, sizeof(f.ssid), "%s", ssid);
    std::memcpy(f.bssid, kBssid, sizeof(f.bssid));
    return AppEvent::handshakeCaptured(f);
}

void test_begin_renders_the_initial_hunting_hud(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    TEST_ASSERT_EQUAL_size_t(1, r.renderCount());  // the HUD is drawn at boot, not on the first tick.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenStatus::Hunting), static_cast<int>(r.last().status));
    TEST_ASSERT_FALSE(r.last().toastActive);
}

void test_status_follows_the_drain(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    step(s, AppEvent::drainStarted(), 100);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenStatus::Working), static_cast<int>(r.last().status));

    const DrainOutcome up = drain(/*associated=*/true);
    step(s, AppEvent::drainCompleted(up), 200);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenStatus::Hunting), static_cast<int>(r.last().status));

    const DrainOutcome offline = drain(/*associated=*/false);
    step(s, AppEvent::drainCompleted(offline), 300);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenStatus::Degraded), static_cast<int>(r.last().status));

    const DrainOutcome hardFault = drain(/*associated=*/true, /*resumeFailed=*/true);
    step(s, AppEvent::drainCompleted(hardFault), 400);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenStatus::Fault), static_cast<int>(r.last().status));
}

void test_counters_accumulate_from_bus_facts(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    step(s, AppEvent::drainCompleted(drain(true, false, /*accepted=*/2, /*duplicate=*/1)), 100);
    TEST_ASSERT_EQUAL_UINT32(3, r.last().uploaded);

    step(s, AppEvent::drainCompleted(drain(true, false, /*accepted=*/1, /*duplicate=*/0)), 200);
    TEST_ASSERT_EQUAL_UINT32(4, r.last().uploaded);  // cumulative, not per-cycle.

    CrackedResult a, b;
    step(s, newPassword(a, "NetA", "pa"), 300);
    step(s, newPassword(b, "NetB", "pb"), 400);
    TEST_ASSERT_EQUAL_UINT32(2, r.last().cracks);
}

void test_sync_line_reflects_the_last_sync(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    SyncOutcome ok;
    ok.ok = true;
    ok.newPasswords = 5;
    step(s, AppEvent::syncCompleted(ok), 100);
    TEST_ASSERT_TRUE(r.last().haveSynced);
    TEST_ASSERT_TRUE(r.last().lastSyncOk);
    TEST_ASSERT_EQUAL_UINT32(5, r.last().lastSyncNew);

    SyncOutcome failed;
    failed.ok = false;
    step(s, AppEvent::syncCompleted(failed), 200);
    TEST_ASSERT_TRUE(r.last().haveSynced);
    TEST_ASSERT_FALSE(r.last().lastSyncOk);  // the latest sync replaces the last.
}

void test_new_password_arms_toast_without_psk(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    CrackedResult p;
    step(s, newPassword(p, "HomeNet", "super-secret-psk"), 100);

    const ScreenView& v = r.last();
    TEST_ASSERT_TRUE(v.toastActive);
    TEST_ASSERT_EQUAL_STRING("HomeNet", v.toastEssid);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", v.toastBssid);
    // The PSK cannot reach the panel: ScreenView has no password field (structural, ADR-0025 #4). If a
    // field were ever added, this would catch a leak into either rendered string.
    TEST_ASSERT_NULL(std::strstr(v.toastEssid, "super-secret-psk"));
    TEST_ASSERT_NULL(std::strstr(v.toastBssid, "super-secret-psk"));
}

void test_toast_clears_after_hold_and_a_repeat_extends_it(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    CrackedResult p;
    step(s, newPassword(p, "HomeNet", "pw"), 100);   // armed at t=100, holds 6000 ms -> until 6100.
    TEST_ASSERT_TRUE(r.last().toastActive);

    s.tick(4000);  // still within the hold.
    TEST_ASSERT_TRUE(r.last().toastActive);

    CrackedResult q;
    step(s, newPassword(q, "HomeNet", "pw"), 4000);  // a repeat re-arms from now -> until 10000.
    s.tick(6100);  // past the FIRST hold but within the extended one.
    TEST_ASSERT_TRUE(r.last().toastActive);

    s.tick(10001);  // past the extended hold.
    TEST_ASSERT_FALSE(r.last().toastActive);
}

void test_non_crack_facts_never_arm_the_toast(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    step(s, AppEvent::drainStarted(), 100);
    step(s, AppEvent::drainCompleted(drain(true)), 200);
    SyncOutcome sync;
    sync.ok = true;
    step(s, AppEvent::syncCompleted(sync), 300);
    step(s, AppEvent::firstSyncSummary(42), 400);  // a seeded backlog raises no banner (ADR-0019 #5).

    TEST_ASSERT_FALSE(r.last().toastActive);
}

void test_capture_arms_a_captured_banner_naming_the_network(void) {
    // The gap this slice closes (ADR-0031): a captured handshake raises a CAPTURED banner, distinct from
    // the crack banner (ToastKind::Captured), naming the network. Fail-before: without the
    // HandshakeCaptured case the banner never arms. The hold is shorter than the crack's (captures are
    // frequent) — armed at t=100, kCapturedHoldMs=2500 -> until 2600.
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    CaptureFact f;
    step(s, captured(f, "lab-ap"), 100);
    const ScreenView& v = r.last();
    TEST_ASSERT_TRUE(v.toastActive);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ToastKind::Captured), static_cast<int>(v.toastKind));
    TEST_ASSERT_EQUAL_STRING("lab-ap", v.toastEssid);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", v.toastBssid);

    s.tick(2000);  // still within the capture hold.
    TEST_ASSERT_TRUE(r.last().toastActive);
    s.tick(2601);  // past the (short) capture hold — a crack's 6000 ms hold would still be up here.
    TEST_ASSERT_FALSE(r.last().toastActive);
}

void test_a_crack_banner_outranks_a_capture_banner(void) {
    // A crack is the rarer, bigger news, so a live CRACKED banner is never replaced by a capture
    // (ADR-0031 #4); once it clears, a fresh capture arms CAPTURED normally.
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    CrackedResult p;
    step(s, newPassword(p, "HomeNet", "pw"), 100);  // CRACKED armed until 6100.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ToastKind::Cracked), static_cast<int>(r.last().toastKind));

    CaptureFact f;
    step(s, captured(f, "lab-ap"), 200);  // a capture arrives mid-CRACKED-hold — must not steal the slot.
    TEST_ASSERT_TRUE(r.last().toastActive);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ToastKind::Cracked), static_cast<int>(r.last().toastKind));
    TEST_ASSERT_EQUAL_STRING("HomeNet", r.last().toastEssid);  // still the crack's network.

    s.tick(6101);  // CRACKED hold elapses.
    TEST_ASSERT_FALSE(r.last().toastActive);
    CaptureFact f2;
    step(s, captured(f2, "lab2"), 6200);  // now a capture is free to arm CAPTURED.
    TEST_ASSERT_TRUE(r.last().toastActive);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ToastKind::Captured), static_cast<int>(r.last().toastKind));
    TEST_ASSERT_EQUAL_STRING("lab2", r.last().toastEssid);
}

void test_heartbeat_toggles_so_a_live_hud_is_not_a_frozen_one(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);
    TEST_ASSERT_TRUE(r.last().heartbeat);  // lit half at the anchor.

    s.tick(1000);  // next half-period.
    TEST_ASSERT_FALSE(r.last().heartbeat);

    s.tick(2000);
    TEST_ASSERT_TRUE(r.last().heartbeat);
}

void test_renders_only_when_the_view_changes(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);
    TEST_ASSERT_EQUAL_size_t(1, r.renderCount());

    s.tick(100);  // same heartbeat half, no fact — nothing changed.
    s.tick(200);
    TEST_ASSERT_EQUAL_size_t(1, r.renderCount());  // no redraw stream on a static view.

    step(s, AppEvent::drainStarted(), 300);  // a real status change (still within the first hb half).
    TEST_ASSERT_EQUAL_size_t(2, r.renderCount());  // exactly one more render.
}

void test_nonprintable_essid_byte_is_filtered_before_display(void) {
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);

    CrackedResult ctrl;
    step(s, newPassword(ctrl, "Ho\x01me", "pw"), 100);  // a control byte in an arbitrary SSID.
    TEST_ASSERT_EQUAL_STRING("Ho?me", r.last().toastEssid);

    CrackedResult ascii;
    step(s, newPassword(ascii, "PlainNet", "pw"), 3000);  // a clean SSID is untouched.
    TEST_ASSERT_EQUAL_STRING("PlainNet", r.last().toastEssid);
}

// --- Live hunt HUD (slice-0034 Scenarios C, D, E; ADR-0033) ----------------------------------------

void test_hunt_line_shows_scan_while_discovering(void) {
    // While Discovering, the live line reports the sweep: phase, parked channel, and how many APs seen.
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    FakeHuntSnapshotSource src;
    src.snapshot.phase = HuntPhase::Discovering;
    src.snapshot.channel = 6;
    src.snapshot.discovered = 3;
    s.setHuntSource(src);
    s.begin(0);

    const ScreenView& v = r.last();
    TEST_ASSERT_TRUE(v.huntShown);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HuntPhase::Discovering), static_cast<int>(v.huntPhase));
    TEST_ASSERT_EQUAL_UINT8(6, v.huntChannel);
    TEST_ASSERT_EQUAL_UINT32(3, v.huntDiscovered);
}

void test_hunt_line_shows_target_and_indicators_while_capturing(void) {
    // While Capturing, the live line names the target and lights the Beacon/M1/M2 indicators it holds.
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    FakeHuntSnapshotSource src;
    src.snapshot.phase = HuntPhase::Capturing;
    std::snprintf(src.snapshot.ssid, sizeof(src.snapshot.ssid), "%s", "lab-ap");
    std::memcpy(src.snapshot.bssid, kBssid, sizeof(src.snapshot.bssid));
    src.snapshot.hasBeacon = true;
    src.snapshot.hasM1 = true;
    src.snapshot.hasM2 = true;  // M3/M4 absent → progress 3-of-5.
    s.setHuntSource(src);
    s.begin(0);

    const ScreenView& v = r.last();
    TEST_ASSERT_TRUE(v.huntShown);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HuntPhase::Capturing), static_cast<int>(v.huntPhase));
    TEST_ASSERT_EQUAL_STRING("lab-ap", v.huntSsid);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", v.huntBssid);  // the fallback label for a hidden SSID.
    TEST_ASSERT_TRUE(v.huntHasBeacon && v.huntHasM1 && v.huntHasM2);
    TEST_ASSERT_FALSE(v.huntHasM3 || v.huntHasM4);
}

void test_hunt_line_filters_a_nonprintable_ssid_byte(void) {
    // An arbitrary SSID octet cannot corrupt the drawn line (mirrors the toast's printable filter).
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    FakeHuntSnapshotSource src;
    src.snapshot.phase = HuntPhase::Capturing;
    std::snprintf(src.snapshot.ssid, sizeof(src.snapshot.ssid), "%s", "Ho\x01me");
    s.setHuntSource(src);
    s.begin(0);
    TEST_ASSERT_EQUAL_STRING("Ho?me", r.last().huntSsid);
}

void test_hunt_line_shows_idle_when_source_reports_not_hunting(void) {
    // A wired source reporting Idle/Quiescing still shows the live line (huntShown) but with the idle
    // phase and no target — the renderer draws the "idle" line, not a stale target row.
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    FakeHuntSnapshotSource src;
    src.snapshot.phase = HuntPhase::Idle;  // not Discovering, not Capturing.
    s.setHuntSource(src);
    s.begin(0);

    const ScreenView& v = r.last();
    TEST_ASSERT_TRUE(v.huntShown);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(HuntPhase::Idle), static_cast<int>(v.huntPhase));
    TEST_ASSERT_FALSE(v.huntHasBeacon || v.huntHasM1 || v.huntHasM2);
    TEST_ASSERT_EQUAL_STRING("", v.huntSsid);
}

void test_no_hunt_source_hides_the_live_line_but_changes_still_render(void) {
    // With no source wired, the pre-0033 HUD renders (no live line). And the new hunt fields are in
    // operator==, so a change in them triggers exactly one redraw (render-on-change still holds).
    FakeScreenRenderer r;
    ScreenToastSurface s(r);
    s.begin(0);
    TEST_ASSERT_FALSE(r.last().huntShown);
    TEST_ASSERT_EQUAL_size_t(1, r.renderCount());

    FakeHuntSnapshotSource src;
    src.snapshot.phase = HuntPhase::Discovering;
    src.snapshot.discovered = 1;
    s.setHuntSource(src);
    s.tick(100);  // now a source is present and the view differs → one redraw.
    TEST_ASSERT_TRUE(r.last().huntShown);
    TEST_ASSERT_EQUAL_size_t(2, r.renderCount());

    src.snapshot.discovered = 2;  // a live-hunt field changed → operator== must catch it and redraw.
    s.tick(150);
    TEST_ASSERT_EQUAL_size_t(3, r.renderCount());
    TEST_ASSERT_EQUAL_UINT32(2, r.last().huntDiscovered);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_renders_the_initial_hunting_hud);
    RUN_TEST(test_status_follows_the_drain);
    RUN_TEST(test_counters_accumulate_from_bus_facts);
    RUN_TEST(test_sync_line_reflects_the_last_sync);
    RUN_TEST(test_new_password_arms_toast_without_psk);
    RUN_TEST(test_toast_clears_after_hold_and_a_repeat_extends_it);
    RUN_TEST(test_non_crack_facts_never_arm_the_toast);
    RUN_TEST(test_capture_arms_a_captured_banner_naming_the_network);
    RUN_TEST(test_a_crack_banner_outranks_a_capture_banner);
    RUN_TEST(test_hunt_line_shows_scan_while_discovering);
    RUN_TEST(test_hunt_line_shows_target_and_indicators_while_capturing);
    RUN_TEST(test_hunt_line_filters_a_nonprintable_ssid_byte);
    RUN_TEST(test_hunt_line_shows_idle_when_source_reports_not_hunting);
    RUN_TEST(test_no_hunt_source_hides_the_live_line_but_changes_still_render);
    RUN_TEST(test_heartbeat_toggles_so_a_live_hud_is_not_a_frozen_one);
    RUN_TEST(test_renders_only_when_the_view_changes);
    RUN_TEST(test_nonprintable_essid_byte_is_filtered_before_display);
    return UNITY_END();
}
