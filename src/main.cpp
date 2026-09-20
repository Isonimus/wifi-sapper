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
#include "config/firmware_version.h"
#include "hal/display/display_hal.h"
#include "hal/display/null_display.h"
#include "hal/serial/serial_channel.h"
#include "net/captive_portal.h"
#include "net/cracked_manifest.h"
#include "net/cracked_store_littlefs.h"
#include "net/capture_store_littlefs.h"
#include "net/capture_queue.h"
#include "net/dashboard.h"
#include "net/maintenance_portal.h"
#include "net/provisioning.h"
#include "net/provisioning_store.h"
#include "net/wifi_station.h"
#include "deauth_probe.h"
#include "hunt_loop.h"
#include "hunt_probe.h"
#include "led_probe.h"
#include "webhook_probe.h"
#include "webhook_capture_probe.h"
#include "rf_discover_probe.h"
#include "rf_sniff_probe.h"
#include "capture_probe.h"
#include "hunt_hud_probe.h"
#include "panel_probe.h"
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

// RGB565 colours the boot splash and the Maintenance panel draw with. The panel-parameter proof
// (the RGB/border/diagonal diagnostic that was the boot face through slice-0005) moved to panel_probe
// with ADR-0041, so its extra primaries live there now, not here.
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGreen = 0x07E0;

// The shipped boot face (ADR-0041): a product splash, not a diagnostic. Name, firmware version, and a
// one-line tagline, drawn with the drawText primitive slice-0026's HUD already proved on this panel.
// A screenless board's NullDisplay makes every call a no-op. The panel-parameter proof it replaced is
// now a bench probe (panel_probe, SAPPER_TEST_PANEL) so no shipped build renders the test pattern.
void drawSplashFrame(IDisplay& d) {
    d.fillScreen(kBlack);
    d.drawText(8, 40, "WiFi Sapper", kGreen, 3);
    char versionLine[24];
    std::snprintf(versionLine, sizeof(versionLine), "v%s", kFirmwareVersion);
    d.drawText(8, 78, versionLine, kWhite, 1);
    d.drawText(8, 96, "autonomous handshake hunter", kWhite, 1);
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
    // Optional Maintenance AP passphrase (ADR-0039): unset expands to "" -> the AP uses the default
    // password. Set SAPPER_TEST_MAINT_PASS to let verify:maintenance assert the AP requires it.
    std::snprintf(seed.maintenancePass, sizeof(seed.maintenancePass), "%s", SAPPER_TEST_MAINT_PASS);
    // seedProvisioning() persists a valid seed or clears any stale triad on an invalid one, so a
    // hooks build with no credential flags routes to the portal instead of inheriting a prior
    // flash's creds (ADR-0008). Host-tested; not a serial path (invariant #7).
    seedProvisioning(seed);
}
#endif

/// Whether the operator is holding BOOT at power-on to enter Maintenance (ADR-0039 decision 2). Reads
/// the board's BOOT/GPIO0 strapping button, active-low. This is the device seam behind the pure
/// decideBootPhase() input (review-only, §4 #6-style boundary); a board without the button (pin -1)
/// never enters Maintenance from here, exactly as before. Repurposes the former inert re-provision stub:
/// re-provisioning a working device is now reachable from the config form, whereas a results viewer had
/// no entry at all.
bool maintenanceRequested() {
#ifdef SAPPER_TEST_HOOKS
    // The physical BOOT-hold cannot be driven by the headless verify, so a build-time hook forces entry
    // (like the credential seed; never a serial path, §4 #7). Compiled out of every shipped build.
    if (SAPPER_TEST_MAINT[0] != '\0') return true;
#endif
    const int8_t pin = kActiveBoard.bootButtonPin;
    if (pin < 0) return false;  // no BOOT button on this board.
    pinMode(pin, INPUT_PULLUP);
    delay(5);  // let the input settle before the single sample.
    return digitalRead(pin) == LOW;  // active-low: pressed reads LOW.
}

/// The Maintenance status screen on a board with a panel (ADR-0039). Connection info + a count only —
/// deliberately NOT the recovered PSKs: the panel is shoulder-surfable, so the passwords stay behind the
/// AP passphrase on the dashboard (the security symmetry of decision 6). A NullDisplay makes it a no-op.
void drawMaintenanceScreen(IDisplay& d, const char* ssid, const char* url, size_t recovered) {
    d.fillScreen(kBlack);
    d.drawText(4, 2, "MAINTENANCE", kWhite, 2);
    char line[40];
    std::snprintf(line, sizeof(line), "AP: %s", ssid);
    d.drawText(4, 30, line, kWhite, 1);
    d.drawText(4, 44, url, kWhite, 1);
    std::snprintf(line, sizeof(line), "%zu recovered", recovered);
    d.drawText(4, 62, line, kGreen, 1);
    d.drawText(4, 84, "power-cycle to resume hunting", kWhite, 1);
}

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

void runMaintenance(const ProvisioningRecord& creds) {
    enterPhase(Phase::Maintenance);

    // Load the persisted recovered results through the §4 #12 seam (read-only in this phase). A store
    // fault is logged loud on serial (the operator/verify's authoritative channel) but does not brick the
    // viewer: the AP still comes up so the operator is never stranded, showing the entries that loaded
    // (0 on a hard fault). This is the one deliberate log-and-continue in the phase — noted in ADR-0039.
    LittleFsCrackedStore crackedStore;
    CrackedManifest manifest(crackedStore);
    if (!crackedStore.begin() || !manifest.begin()) {
        Serial.println("[FATAL] maintenance: could not load recovered results");
    }

    // The persisted capture-queue depth for the summary: enumerate pending captures through the store.
    size_t queueDepth = 0;
    LittleFsCaptureStore captureStore;
    if (captureStore.begin()) {
        PendingCapture pending[kCaptureStoreScanCapacity];
        size_t count = 0;
        if (captureStore.listPending(pending, kCaptureStoreScanCapacity, count)) queueDepth = count;
    }

    MaintenancePortal portal(manifest, creds.deauthEnabled, queueDepth, creds.maintenancePass);
    if (!portal.begin()) {
        Serial.println("[FATAL] maintenance: SoftAP failed to start");
        return;
    }

    drawMaintenanceScreen(display(), portal.apSsid(), "http://192.168.4.1/", manifest.size());
    display().present();

    char banner[96];
    std::snprintf(banner, sizeof(banner), "[MAINT] ssid=%s url=http://%s/ recovered=%u",
                  portal.apSsid(), WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned>(manifest.size()));
    Serial.println(banner);

    // Serve until an explicit resume (a power-cycle without BOOT re-enters Station via the boot gate) or
    // the no-activity backstop fires, protecting the endless-hunt identity of a walked-away unit
    // (ADR-0039 decision 7). Both leave by rebooting; the radio comes back up clean for the hunt.
    uint32_t lastActivity = millis();
    for (;;) {
        portal.handle();
        g_channel.pump();  // keep `state`/`ping` answered while parked in Maintenance (§4 #3).
        if (portal.consumeActivity()) lastActivity = millis();
        if (maintenanceBackstopDue(lastActivity, millis())) {
            Serial.println("[MAINT] no-activity backstop reached — rebooting into Station");
            delay(50);  // let the line flush before the reset.
            ESP.restart();
        }
        delay(5);
    }
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
        if (decideBootPhase(/*hasValidStoredCreds=*/true, staFail, /*maintenanceRequested=*/false) ==
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
    drawSplashFrame(d);
    d.present();

#ifdef SAPPER_TEST_HOOKS
    seedTestCredentials();
#endif

    // Bench RF probes (slice-0012/0014/0016, ADR-0011/ADR-0013/ADR-0015): each takes over the device
    // to prove a radio seam on air, skipping the normal boot. The hunt probe (the full endless loop)
    // gets first refusal, then the discover probe (channel hopping + AP discovery), then the
    // fixed-channel sniff probe. All are inactive unless their env var is set and compiled out of
    // every shipped build, so these return false there and boot proceeds normally.
    if (panelProbeBegin(d)) return;    // renders the ADR-0002 §5 panel proof over the splash (ADR-0041).
    if (captureProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
    if (huntHudProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
    if (screenProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
    if (webhookProbeBegin()) return;
    if (webhookCaptureProbeBegin()) return;
    if (ledProbeBegin()) return;
    if (syncProbeBegin()) return;
    if (uploadProbeBegin()) return;
    if (huntProbeBegin()) return;
    if (rfDiscoverProbeBegin()) return;
    if (rfSniffProbeBegin()) return;
    if (deauthProbeBegin()) return;  // transmits deauth + captures the forced handshake (ADR-0027).

    ProvisioningRecord creds = {};
    const bool hasCreds = loadProvisioning(creds);
    const Phase entry = decideBootPhase(hasCreds, /*staFailCount=*/0, maintenanceRequested());
    if (entry == Phase::Provisioning) {
        startPortal();
    } else if (entry == Phase::Maintenance) {
        runMaintenance(creds);  // BOOT held at power-on: serve the results dashboard, never returns.
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
    if (huntHudProbeActive()) {  // bench live-hunt-HUD verify owns the device; normal boot is skipped.
        g_channel.pump();  // answers `dump` — pump the channel so serial commands run while it renders.
        huntHudProbePump();
        delay(5);
        return;
    }
    if (panelProbeActive()) {  // bench panel proof owns the device; the normal boot loop is skipped.
        g_channel.pump();  // like the screen verify, this one needs `dump` answered — pump the channel so
                           // serial commands run while the device parks on the diagnostic frame.
        panelProbePump();
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
    if (webhookCaptureProbeActive()) {  // bench capture-push verify owns the device; normal boot skipped.
        webhookCaptureProbePump();
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
