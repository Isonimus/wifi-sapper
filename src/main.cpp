/**
 * @file main.cpp
 * @brief Boot state machine: provision (captive portal) or associate (STA + NTP), then idle at
 *        Ready where the capture engine mounts later (ADR-0001, ADR-0002, ADR-0003, ADR-0006).
 *
 * This owns the live boot `Phase` (slice-0007). At boot it seeds the display, decides via the
 * pure boot gate whether stored credentials send it straight to the station or the portal, and
 * drives the phases forward — announcing each on the serial `[STATE]` line. There is still no
 * capture engine (slice-3/4); `Ready` is the headless steady state this slice reaches.
 */
#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

#include "config/active_board.h"
#include "hal/display/display_hal.h"
#include "hal/display/null_display.h"
#include "hal/serial/serial_channel.h"
#include "net/captive_portal.h"
#include "net/provisioning.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"
#include "deauth_probe.h"
#include "hunt_loop.h"
#include "hunt_probe.h"
#include "led_probe.h"
#include "webhook_probe.h"
#include "rf_discover_probe.h"
#include "rf_sniff_probe.h"
#include "capture_probe.h"
#include "screen_probe.h"
#include "sync_probe.h"
#include "upload_probe.h"
#if SAPPER_BOARD_HAS_DISPLAY
#include "hal/display/lgfx_display.h"
#endif

namespace {
using namespace sapper;

/// The active display, resolved at compile time from the board's capability (ADR-0002 #4).
/// A screenless board compiles no LGFX_* class and gets the null display.
IDisplay& display() {
#if SAPPER_BOARD_HAS_DISPLAY
    static LgfxDisplay instance(kActiveBoard);
#else
    static NullDisplay instance;
#endif
    return instance;
}

// RGB565 primaries. The R/G/B blocks make a colour-order or byte-order error obvious in the
// dump artifact; the border makes an offset or clipping error obvious; the diagonal makes a
// mirror or rotation error obvious. Together they confirm the ADR-0002 §5 panel parameters.
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kBlue = 0x001F;

void fillRect(IDisplay& d, int x0, int y0, int w, int h, uint16_t color) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            d.drawPixel(x, y, color);
        }
    }
}

// Boot self-test / splash. It doubles as the ADR-0002 §5 panel proof; a real status surface is
// slice-7 (alert surfaces). A screenless board's NullDisplay makes every call a no-op.
void drawBringupFrame(IDisplay& d) {
    const int w = d.width();
    const int h = d.height();
    d.fillScreen(kBlack);

    fillRect(d, 3, 3, 20, 20, kRed);
    fillRect(d, 25, 3, 20, 20, kGreen);
    fillRect(d, 47, 3, 20, 20, kBlue);

    for (int x = 0; x < w; ++x) {
        d.drawPixel(x, 0, kWhite);
        d.drawPixel(x, h - 1, kWhite);
    }
    for (int y = 0; y < h; ++y) {
        d.drawPixel(0, y, kWhite);
        d.drawPixel(w - 1, y, kWhite);
    }

    const int span = w < h ? w : h;
    for (int i = 0; i < span; ++i) {
        d.drawPixel(i * (w - 1) / (span - 1), i * (h - 1) / (span - 1), kWhite);
    }
}

// The live boot phase. main.cpp owns it; SerialChannel reads it by reference so `[STATE]` always
// reports the true phase (ADR-0006 #3). It starts at Provisioning; the boot gate may advance it.
Phase g_phase = Phase::Provisioning;
SerialChannel g_channel(display(), g_phase);
CaptivePortal g_portal;

/// Pump the serial channel during a pre-engine blocking wait (CLAUDE.md §4 invariant #3), passed
/// as the tick into the bounded STA/NTP waits so `state`/`ping` stay answered during boot.
void pumpSerial() { g_channel.pump(); }

/// Advance the reported phase and announce it, so every transition emits a `[STATE]` line the
/// verify script observes (slice-0007 Scenarios C, D).
void enterPhase(Phase phase) {
    g_phase = phase;
    g_channel.announce();
}

#ifdef SAPPER_TEST_HOOKS
void seedTestCredentials() {
    // ADR-0006 #7 reconciled with §4 invariant #7: the bench harness injects credentials through
    // the persist seam at COMPILE TIME (build flags), never over serial. A hooks build missing a
    // flag stores an invalid (empty) triad, which the gate simply routes to the portal.
    ProvisioningRecord seed = {};
    std::snprintf(seed.ssid, sizeof(seed.ssid), "%s", SAPPER_TEST_WIFI_SSID);
    std::snprintf(seed.pass, sizeof(seed.pass), "%s", SAPPER_TEST_WIFI_PASS);
    std::snprintf(seed.key, sizeof(seed.key), "%s", SAPPER_TEST_WPASEC_KEY);
    // Optional push webhook URL (ADR-0023): unset expands to "" → the webhook stays disabled, exactly
    // as an unset credential routes to the portal. Set SAPPER_TEST_WEBHOOK_URL to run verify:webhook.
    std::snprintf(seed.webhookUrl, sizeof(seed.webhookUrl), "%s", SAPPER_TEST_WEBHOOK_URL);
    // seedProvisioning() persists a valid seed or clears any stale triad on an invalid one, so a
    // hooks build with no credential flags routes to the portal instead of inheriting a prior
    // flash's creds (ADR-0008). Host-tested; not a serial path (invariant #7).
    seedProvisioning(seed);
}
#endif

/// Whether the operator is asking to re-provision a device that already has valid credentials.
/// The physical trigger (button/keyboard hold at boot) needs an input HAL this repo does not yet
/// have, so it is deferred (LEDGER, ADR-0006); the STA-fail fallback in runStationBoot() already
/// re-opens the portal automatically when a provisioned device cannot reach its network.
bool reprovisionRequested() { return false; }

void startPortal() {
    enterPhase(Phase::Provisioning);
    if (!g_portal.begin()) {
        // Fail loud: no AP means no way to provision (the verify script watches for [FATAL]).
        Serial.println("[FATAL] captive portal failed to start");
        return;
    }
    char banner[96];
    std::snprintf(banner, sizeof(banner), "[PORTAL] ssid=%s pass=%s url=http://%s/",
                  g_portal.apSsid(), kSoftApPassword, WiFi.softAPIP().toString().c_str());
    Serial.println(banner);
}

void runStationBoot(const ProvisioningRecord& creds) {
    for (uint8_t staFail = 0;;) {
        enterPhase(Phase::StationConnect);
        if (connectStation(creds.ssid, creds.pass, &pumpSerial)) {
            enterPhase(Phase::TimeSync);
            if (syncClock(&pumpSerial)) {
                enterPhase(Phase::Ready);  // headless steady state.
                // Start the shipped hunt→enqueue→drain→upload loop (ADR-0017 decision #8). It releases
                // the boot association and re-owns the radio promiscuously; loop() pumps it from here.
                if (!huntLoopBegin(creds, {}, &display())) {  // pass the panel so the status HUD renders.
                    Serial.println("[FATAL] hunt/upload loop failed to start");
                }
                return;
            }
        }
        ++staFail;
        // Budget spent -> fall back to the portal for reconfiguration (fail loud, no silent loop).
        if (decideBootPhase(/*hasValidStoredCreds=*/true, staFail, /*reprovisionRequested=*/false) ==
            Phase::Provisioning) {
            startPortal();
            return;
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(200);  // let the S3 USB-CDC endpoint enumerate before the first line

    IDisplay& d = display();
    if (!d.begin()) {
        Serial.println("[FATAL] display init failed");
    }
    drawBringupFrame(d);
    d.present();

#ifdef SAPPER_TEST_HOOKS
    seedTestCredentials();
#endif

    // Bench RF probes (slice-0012/0014/0016, ADR-0011/ADR-0013/ADR-0015): each takes over the device
    // to prove a radio seam on air, skipping the normal boot. The hunt probe (the full endless loop)
    // gets first refusal, then the discover probe (channel hopping + AP discovery), then the
    // fixed-channel sniff probe. All are inactive unless their env var is set and compiled out of
    // every shipped build, so these return false there and boot proceeds normally.
    if (captureProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
    if (screenProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
    if (webhookProbeBegin()) return;
    if (ledProbeBegin()) return;
    if (syncProbeBegin()) return;
    if (uploadProbeBegin()) return;
    if (huntProbeBegin()) return;
    if (rfDiscoverProbeBegin()) return;
    if (rfSniffProbeBegin()) return;
    if (deauthProbeBegin()) return;  // transmits deauth + captures the forced handshake (ADR-0027).

    ProvisioningRecord creds = {};
    const bool hasCreds = loadProvisioning(creds);
    const Phase entry = decideBootPhase(hasCreds, /*staFailCount=*/0, reprovisionRequested());
    if (entry == Phase::Provisioning) {
        startPortal();
    } else {
        runStationBoot(creds);  // hunt_loop copies the creds it needs; this local may go out of scope.
    }
}

void loop() {
    if (captureProbeActive()) {  // bench capture-notify verify owns the device; normal boot is skipped.
        g_channel.pump();  // like the screen verify, this one answers `dump` — pump the channel so the
                           // serial control commands are dispatched while the probe renders the banner.
        captureProbePump();
        delay(5);
        return;
    }
    if (screenProbeActive()) {  // bench screen verify owns the device; the normal boot loop is skipped.
        g_channel.pump();  // unlike the other probes, this verify needs `dump` answered — pump the channel
                           // so the serial control commands are dispatched while the probe renders.
        screenProbePump();
        delay(5);
        return;
    }
    if (webhookProbeActive()) {  // bench webhook verify owns the device; the normal boot loop is skipped.
        webhookProbePump();
        delay(5);
        return;
    }
    if (ledProbeActive()) {  // bench LED verify owns the device; the normal boot loop is skipped.
        ledProbePump();
        delay(5);
        return;
    }
    if (syncProbeActive()) {  // bench sync verify owns the device; the normal boot loop is skipped.
        syncProbePump();
        delay(5);
        return;
    }
    if (uploadProbeActive()) {  // bench upload verify owns the device; the normal boot loop is skipped.
        uploadProbePump();
        delay(5);
        return;
    }
    if (huntProbeActive()) {  // bench RF probe owns the device; the normal boot loop is skipped.
        huntProbePump();
        delay(5);
        return;
    }
    if (rfDiscoverProbeActive()) {  // bench RF probe owns the device; the normal boot loop is skipped.
        rfDiscoverProbePump();
        delay(5);
        return;
    }
    if (rfSniffProbeActive()) {  // bench RF probe owns the device; the normal boot loop is skipped.
        rfSniffProbePump();
        delay(5);
        return;
    }
    if (deauthProbeActive()) {  // bench deauth probe owns the device; the normal boot loop is skipped.
        deauthProbePump();
        delay(5);
        return;
    }

    g_channel.pump();

    if (g_phase == Phase::Provisioning) {
        g_portal.handle();
        if (g_portal.provisioned()) {
            Serial.println("[PORTAL] saved; rebooting");
            delay(500);  // let the HTTP success response flush before the AP drops.
            ESP.restart();
        }
    } else if (g_phase == Phase::Ready) {
        huntLoopPump();  // the shipped endless hunt→enqueue→drain→upload loop (ADR-0017 decision #8).
    }
    delay(5);
}
