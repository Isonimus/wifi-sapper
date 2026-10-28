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
#include <new>             // placement new: PSRAM placement of the Maintenance cracked manifest (ADR-0080).
#include <esp_heap_caps.h>  // heap_caps_malloc(MALLOC_CAP_SPIRAM): the PSRAM allocation itself (ADR-0080).

#include "config/active_board.h"
#include "config/button_hold.h"
#include "config/firmware_version.h"
#include "config/maintenance_intent.h"
#include "hal/display/display_hal.h"
#include "hal/display/null_display.h"
#include "hal/input/tca8418_keyboard.h"
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
#include "surface/wordmark.h"
#include "deauth_probe.h"
#include "hunt_loop.h"
#include "hunt_probe.h"
#include "keyboard_probe.h"
#include "led_probe.h"
#include "webhook_probe.h"
#include "webhook_capture_probe.h"
#include "rf_discover_probe.h"
#include "rf_sniff_probe.h"
#include "rssi_probe.h"
#include "capture_probe.h"
#include "hunt_hud_probe.h"
#include "panel_probe.h"
#include "screen_probe.h"
#include "sync_probe.h"
#include "upload_probe.h"
#if SAPPER_BOARD_HAS_DISPLAY
#if defined(SAPPER_DISPLAY_DIRECT_PANEL)
#include "hal/display/direct_panel_display.h"   // ADR-0084: panel too large for a full-frame sprite, no PSRAM
#else
#include "hal/display/lgfx_display.h"
#endif
#endif

namespace {
using namespace sapper;

/// The active display, resolved at compile time from the board's capability (ADR-0002 #4). A screenless
/// board compiles no LGFX_* class and gets the null display; a display board whose panel is too large for a
/// full-frame sprite and has no PSRAM gets the buffer-free direct renderer (SAPPER_DISPLAY_DIRECT_PANEL,
/// ADR-0084); every other display board keeps the sprite-backed LgfxDisplay.
IDisplay& display() {
#if SAPPER_BOARD_HAS_DISPLAY
#if defined(SAPPER_DISPLAY_DIRECT_PANEL)
    static DirectPanelDisplay instance(kActiveBoard);
#else
    static LgfxDisplay instance(kActiveBoard);
#endif
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
constexpr uint16_t kCyan = 0x07FF;  // the banner's tagline hue (slice-0097).

// The shipped boot face (ADR-0041): a product splash, not a diagnostic. Name, firmware version, and a
// one-line tagline, drawn with the drawText primitive slice-0026's HUD already proved on this panel.
// A screenless board's NullDisplay makes every call a no-op. The panel-parameter proof it replaced is
// now a bench probe (panel_probe, SAPPER_TEST_PANEL) so no shipped build renders the test pattern.
void drawSplashFrame(IDisplay& d) {
    d.fillScreen(kBlack);
    // The product wordmark: white glyphs over a banner-magenta drop shadow (slice-0097); the tagline in the
    // banner's cyan. The version line stays plain white.
    drawWordmark(d, 8, 40, "WiFi Sapper", 3);
    char versionLine[24];
    std::snprintf(versionLine, sizeof(versionLine), "v%s", kFirmwareVersion);
    d.drawText(8, 78, versionLine, kWhite, 1);
    d.drawText(8, 96, "autonomous handshake hunter", kCyan, 1);
}

// The live boot phase. main.cpp owns it; SerialChannel reads it by reference so `[STATE]` always
// reports the true phase (ADR-0006 #3). It starts at Provisioning; the boot gate may advance it.
Phase g_phase = Phase::Provisioning;
SerialChannel g_channel(display(), g_phase);
CaptivePortal g_portal;

// The runtime keyboard, brought up on entering Ready so `loop()` can watch for the Maintenance key
// (ADR-0057). Idle on any board without a keyboard (begin() is gated on the profile's InputKind).
Tca8418Keyboard g_keyboard;

// The reboot-into-Maintenance intent (ADR-0057 decision 3). RTC_NOINIT_ATTR: this word survives the
// software ESP.restart() the `m` keypress issues, but is *undefined* after a power-on/brownout — which
// is exactly why the pure decision also gates on esp_reset_reason() == ESP_RST_SW. Not static: a
// file-scope global lands in the .rtc_noinit section the attribute selects.
RTC_NOINIT_ATTR uint32_t g_maintenanceIntentWord;

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

/// Whether this clean boot should enter Maintenance (ADR-0057, superseding ADR-0053). The signal is a
/// runtime `m` keypress that persisted a reboot-intent and restarted the device — NOT a boot-button
/// poll: the Cardputer ADV has no runtime-pressable GPIO0 button (GPIO0 is a boot strap only; the
/// field diagnostic held it 8 s and read HIGH throughout), so ADR-0053's windowed GPIO0 poll was
/// unreachable. Reads and one-shot-consumes the RTC intent word through the pure decision, so a
/// power-cycle (undefined RTC word, non-ESP_RST_SW reset) never enters Maintenance. Maintenance still
/// enters only from a clean boot, so §4 #21 holds; the device seam is just the RTC word + reset reason,
/// the decision is the host-tested consumeMaintenanceIntent().
bool maintenanceRequested() {
#ifdef SAPPER_TEST_HOOKS
    // The physical press cannot be driven by the headless verify, so a build-time hook forces entry
    // (like the credential seed; never a serial path, §4 #7). Compiled out of every shipped build.
    if (SAPPER_TEST_MAINT[0] != '\0') return true;
#endif
    const MaintenanceIntentRead intent =
        consumeMaintenanceIntent(esp_reset_reason(), g_maintenanceIntentWord);
    g_maintenanceIntentWord = intent.clearedWord;  // one-shot: consume before the phase is decided, so a
                                                    // crash inside Maintenance cannot loop back into it.
    return intent.enterMaintenance;
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

// The first-boot provisioning instructions on a board with a panel (ADR-0089, refining §4 #22's
// splash-only boot face). Guided, newbie-first: an operator who flashed from m5burner never saw the
// README, so the panel itself must say how to join the SoftAP and reach the setup page. The password
// shown is the fixed PUBLIC default kSoftApPassword, not a per-device secret, so putting it on the
// shoulder-surfable panel leaks nothing — the deliberate opposite of §4 #21's per-device Maintenance
// PSKs, which stay OFF the panel; do not "fix" this by hiding the default password. A NullDisplay makes
// every call a no-op (ADR-0045). Fixed-coordinate like its splash/Maintenance siblings; a resolution-
// aware pass over all three boot faces is deferred (LEDGER).
void drawProvisioningScreen(IDisplay& d, const char* ssid, const char* password, const char* url) {
    d.fillScreen(kBlack);
    d.drawText(4, 2, "SAPPER SETUP", kWhite, 2);
    char line[48];
    d.drawText(4, 28, "1. Join WiFi:", kWhite, 1);
    std::snprintf(line, sizeof(line), "   %s", ssid);
    d.drawText(4, 42, line, kGreen, 1);
    std::snprintf(line, sizeof(line), "   pass: %s", password);
    d.drawText(4, 56, line, kGreen, 1);
    d.drawText(4, 78, "2. Open in browser:", kWhite, 1);
    std::snprintf(line, sizeof(line), "   %s", url);
    d.drawText(4, 92, line, kGreen, 1);
}

void startPortal(IDisplay* panel) {
    enterPhase(Phase::Provisioning);
    if (!g_portal.begin()) {
        // Fail loud: no AP means no way to provision (the verify script watches for [FATAL]).
        Serial.println("[FATAL] captive portal failed to start");
        return;
    }
    char url[32];
    std::snprintf(url, sizeof(url), "http://%s/", WiFi.softAPIP().toString().c_str());
    char banner[96];
    std::snprintf(banner, sizeof(banner), "[PORTAL] ssid=%s pass=%s url=%s",
                  g_portal.apSsid(), kSoftApPassword, url);
    Serial.println(banner);
    // Mirror the join details on the panel for a screen-equipped first boot (ADR-0089): the serial banner
    // above serves the headless operator, this serves the one who flashed from m5burner and never saw the
    // README. nullptr is the failed/absent panel (ADR-0045) — skipped like the hunt HUD's headless path.
    if (panel != nullptr) {
        drawProvisioningScreen(*panel, g_portal.apSsid(), kSoftApPassword, url);
        panel->present();
    }
}

void runMaintenance(const ProvisioningRecord& creds, IDisplay& panel) {
    enterPhase(Phase::Maintenance);

    // Load the persisted recovered results through the §4 #12 seam (read-only in this phase). A store
    // fault is logged loud on serial (the operator/verify's authoritative channel) but does not brick the
    // viewer: the AP still comes up so the operator is never stranded, showing the entries that loaded
    // (0 on a hard fault). This is the one deliberate log-and-continue in the phase — noted in ADR-0039.
    //
    // `static`, NOT a stack local: CrackedManifest embeds `CrackedEntry entries_[kMaxCrackedEntries]`
    // (256 × ~112 B ≈ 28 KB), which overflows the ~8 KB loopTask stack the instant it is constructed —
    // a stack-canary panic in the ctor, measured on-device (this is why Maintenance rebooted before the
    // SoftAP ever came up once ADR-0057 finally made it reachable). It mirrors the hunt path's g_manifest
    // (hunt_loop.cpp): PSRAM heap on a PSRAM board (ADR-0080), internal DRAM heap on a no-PSRAM classic ESP32
    // (ADR-0082), static .bss on a no-PSRAM board whose budget fits (the S3 Cardputer). runMaintenance runs
    // once and does not return on the happy path, so a static local/pointer is the natural lifetime; the store
    // is static too so the manifest's reference to it stays valid even on the early-return fault path below.
    static LittleFsCrackedStore crackedStore;
#if defined(BOARD_HAS_PSRAM) || defined(SAPPER_MANIFEST_HEAP_INTERNAL)
    // Heap boards allocate the ~28KB manifest at runtime and fail loud + reboot on a null alloc: PSRAM on a
    // PSRAM board (ADR-0080), the INTERNAL DRAM heap on a no-PSRAM classic ESP32 (ADR-0082, where static .bss
    // overflowed dram0_0_seg by 27056 bytes, slice-0083). Internal-heap placement fits here for the same
    // reason as the hunt mirror: hunt and Maintenance are mutually-exclusive boot phases, so only this one
    // manifest is live during Maintenance. A DRAM fallback would be the very block that overflowed the link,
    // so there is nothing safe to continue with (unlike the load-fault below, which keeps the viewer up on an
    // empty manifest). Allocated once; runMaintenance does not return on the happy path.
    static CrackedManifest* manifestPtr = nullptr;
    if (manifestPtr == nullptr) {
#if defined(BOARD_HAS_PSRAM)
        void* manifestMem = heap_caps_malloc(sizeof(CrackedManifest), MALLOC_CAP_SPIRAM);
#else
        void* manifestMem = heap_caps_malloc(sizeof(CrackedManifest), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        if (manifestMem == nullptr) {
            Serial.println("[FATAL] maintenance: cracked manifest heap allocation failed");
            delay(50);  // let the [FATAL] line flush before the reset (as every ESP.restart() below does).
            ESP.restart();
        }
        manifestPtr = new (manifestMem) CrackedManifest(crackedStore);
    }
    CrackedManifest& manifest = *manifestPtr;
#else
    // No-PSRAM boards whose static budget fits (the roomy S3 Cardputer) keep the manifest a static .bss object,
    // exactly as it shipped (ADR-0080).
    static CrackedManifest manifestStorage(crackedStore);
    CrackedManifest& manifest = manifestStorage;
#endif
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

    MaintenancePortal portal(manifest, creds, queueDepth);
    if (!portal.begin()) {
        Serial.println("[FATAL] maintenance: SoftAP failed to start");
        return;
    }

    drawMaintenanceScreen(panel, portal.apSsid(), "http://192.168.4.1/", manifest.size());
    panel.present();

    char banner[96];
    std::snprintf(banner, sizeof(banner), "[MAINT] ssid=%s url=http://%s/ recovered=%u",
                  portal.apSsid(), WiFi.softAPIP().toString().c_str(),
                  static_cast<unsigned>(manifest.size()));
    Serial.println(banner);

    // Serve until an explicit resume (a plain power-cycle re-enters Station via the boot gate: the RTC
    // intent is not set, so the next boot hunts) or the no-activity backstop fires, protecting the
    // endless-hunt identity of a walked-away unit (ADR-0039 decision 7). Both leave by rebooting; the
    // radio comes back up clean for the hunt.
    uint32_t lastActivity = millis();
    for (;;) {
        portal.handle();
        g_channel.pump();  // keep `state`/`ping` answered while parked in Maintenance (§4 #3).
        if (portal.consumeActivity()) lastActivity = millis();
        // A control (resume, or a persisted re-provision) asked to reboot into Station. The handler
        // already sent its acknowledgement; reboot here, after handle() flushed it, never inside the
        // handler (ADR-0043 decision 3, §4 #23) — a reboot-scoped intent, not a live engine action.
        if (portal.rebootRequested()) {
            Serial.println("[MAINT] control requested resume — rebooting into Station");
            delay(500);  // let the HTTP response flush before the AP drops.
            ESP.restart();
        }
        if (maintenanceBackstopDue(lastActivity, millis())) {
            Serial.println("[MAINT] no-activity backstop reached — rebooting into Station");
            delay(50);  // let the line flush before the reset.
            ESP.restart();
        }
        delay(5);
    }
}

void runStationBoot(const ProvisioningRecord& creds, IDisplay* panel) {
    for (uint8_t staFail = 0;;) {
        enterPhase(Phase::StationConnect);
        if (connectStation(creds.ssid, creds.pass, &pumpSerial)) {
            enterPhase(Phase::TimeSync);
            if (syncClock(&pumpSerial)) {
                enterPhase(Phase::Ready);  // headless steady state.
                // Start the shipped hunt→enqueue→drain→upload loop (ADR-0017 decision #8). It releases
                // the boot association and re-owns the radio promiscuously; loop() pumps it from here.
                // `panel` is null when the panel failed to init, so the HUD is skipped and the hunt runs
                // on headless — the proven "no display" path (ADR-0045 decision 3).
                if (!huntLoopBegin(creds, {}, panel)) {  // pass the panel so the status HUD renders.
                    Serial.println("[FATAL] hunt/upload loop failed to start");
                }
                // Bring up the keyboard so loop() can watch for the Maintenance key (ADR-0057). Gated on
                // the board declaring a keyboard; the driver is ADV-specific (decision 7). A failure is
                // logged loud but does not brick hunting (the primary mission) — it only means runtime
                // Maintenance entry is unavailable, never a swallowed error (§3 fail loud, not fail hard).
                if (kActiveBoard.input == InputKind::Keyboard && !g_keyboard.begin()) {
                    Serial.println("[KB] keyboard init failed — runtime Maintenance entry unavailable");
                }
                // Prepare the entry button on a button board so loop() can watch for the Maintenance hold
                // (ADR-0078). The Stick's BtnB is an input-only pad with an external pull-up, so it reads as
                // INPUT, never INPUT_PULLUP (which would error on an input-only pad — dragon-ball measurement).
                if (kActiveBoard.input == InputKind::Buttons && kActiveBoard.bootButtonPin >= 0) {
                    pinMode(kActiveBoard.bootButtonPin, INPUT);
                }
                return;
            }
        }
        ++staFail;
        // Budget spent -> fall back to the portal for reconfiguration (fail loud, no silent loop).
        if (decideBootPhase(/*hasValidStoredCreds=*/true, staFail, /*maintenanceRequested=*/false) ==
            Phase::Provisioning) {
            startPortal(panel);  // panel is the nullable HUD handle (nullptr on a failed/absent panel).
            return;
        }
    }
}

/// The per-board Maintenance-entry gesture, polled by loop() in Ready (ADR-0078). Selected at compile
/// time by the active profile's InputKind (capability, not board identity — ADR-0002): a keyboard board
/// returns the TCA8418 `m`-press (ADR-0057); a button board returns a deliberate debounced hold of the
/// entry button (pure `decideButtonHold`); any other board has no runtime entry. Returns true at most
/// once per gesture, after which loop() persists the reboot-intent and restarts.
bool maintenanceEntryRequested() {
    if constexpr (kActiveBoard.input == InputKind::Keyboard) {
        return g_keyboard.maintenanceKeyPressed();
    } else if constexpr (kActiveBoard.input == InputKind::Buttons) {
        if (kActiveBoard.bootButtonPin < 0) return false;  // Buttons board with no declared entry button.
        static ButtonHoldState holdState;  // one hold-timer for the single entry button.
        const bool pressed = digitalRead(kActiveBoard.bootButtonPin) == LOW;  // active-low pad.
        return decideButtonHold(pressed, millis(), kMaintenanceHoldMs, holdState);
    } else {
        return false;  // None/Touch: no entry gesture built yet (ADR-0078 scope).
    }
}

}  // namespace

void setup() {
    // Assert the board's software power-latch before anything else or the device powers off the moment the
    // power button is released (the M5StickC Plus2's GPIO4). A board with no latch (powerHoldPin == -1, the
    // USB-powered Cardputer) is unchanged — this is a no-op there (ADR-0002 board fact, ADR-0078/slice-0079).
    if (kActiveBoard.powerHoldPin >= 0) {
        pinMode(kActiveBoard.powerHoldPin, OUTPUT);
        digitalWrite(kActiveBoard.powerHoldPin, HIGH);
    }

    Serial.begin(115200);
    delay(200);  // let the S3 USB-CDC endpoint enumerate before the first line

    IDisplay& d = display();
    const bool panelReady = d.begin();
    if (!panelReady) {
        // Fail loud, then fail safe: a real panel init failure is announced once here and the boot
        // continues fully headless. Every surface below draws through `panel`, never `d` directly, so
        // a failed panel is treated exactly as an absent one — the proven screenless path — instead of
        // half-drawing on the guarded-but-dead LovyanGFX buffer (ADR-0045, §3 fail-loud + #24).
        Serial.println("[FATAL] display init failed — panel disabled, continuing headless");
    }
    static NullDisplay nullPanel;  // the fallback the surfaces no-op onto when the real panel failed.
    IDisplay& panel = panelAfterInit(d, panelReady, nullPanel);
    drawSplashFrame(panel);
    panel.present();

#ifdef SAPPER_TEST_HOOKS
    seedTestCredentials();
#endif

    // Bench RF probes (slice-0012/0014/0016, ADR-0011/ADR-0013/ADR-0015): each takes over the device
    // to prove a radio seam on air, skipping the normal boot. The hunt probe (the full endless loop)
    // gets first refusal, then the discover probe (channel hopping + AP discovery), then the
    // fixed-channel sniff probe. All are inactive unless their env var is set and compiled out of
    // every shipped build, so these return false there and boot proceeds normally.
    // The panel-rendering bench probes exist only to draw a surface to the panel and dump it, so they
    // cannot run without a live panel. On an init failure the [FATAL] above is the loud signal (and the
    // verify script's missing artifact is the fail); skip them rather than push to a dead panel — for
    // panel_probe that push is present() through an uninitialised driver, the very hardware half-draw #24
    // forbids, during the exact bench session meant to diagnose it. Inside this guard `panel` == `d`
    // (the real, live panel). The non-panel probes below (network/LED/RF) need no panel (ADR-0045 #4).
    if (panelReady) {
        if (panelProbeBegin(d)) return;    // renders the ADR-0002 §5 panel proof over the splash (ADR-0041).
        if (captureProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
        if (huntHudProbeBegin(d)) return;  // renders to the panel d already brought up (one canvas).
        if (screenProbeBegin(d)) return;   // renders to the panel d already brought up (one canvas).
    }
    if (webhookProbeBegin()) return;
    if (webhookCaptureProbeBegin()) return;
    if (ledProbeBegin()) return;
    if (syncProbeBegin()) return;
    if (uploadProbeBegin()) return;
    if (huntProbeBegin()) return;
    if (rssiProbeBegin()) return;  // armed hunt + RSSI tap: correlates beacon RSSI with capture (ADR-0067).
    if (rfDiscoverProbeBegin()) return;
    if (rfSniffProbeBegin()) return;
    if (deauthProbeBegin()) return;  // transmits deauth + captures the forced handshake (ADR-0027).
    if (keyboardProbeBegin()) return;  // reads the TCA8418 to confirm the Maintenance keycode (ADR-0057).

    ProvisioningRecord creds = {};
    const bool hasCreds = loadProvisioning(creds);
    // Read AND consume the intent every boot — never behind the hasCreds gate. The consume is the
    // one-shot clear (`consumeMaintenanceIntent`), and it must run unconditionally so an unprovisioned
    // cold boot cannot leave undefined RTC-noinit garbage sitting in the word: the very first
    // *provisioned* boot is itself an ESP_RST_SW (the captive-portal save reboots), so a short-circuit
    // here would let cold-boot garbage that happens to equal the sentinel enter Maintenance on that
    // first provisioned boot, having never been cleared. Only the resulting *decision* is gated on
    // hasCreds (decideBootPhase ignores the signal when unprovisioned anyway) — ADR-0057 decision 3.
    const bool intentRequested = maintenanceRequested();
    const bool maint = hasCreds && intentRequested;
    const Phase entry = decideBootPhase(hasCreds, /*staFailCount=*/0, maint);
    if (entry == Phase::Provisioning) {
        startPortal(panelReady ? &display() : nullptr);  // draw the setup screen on a live panel (ADR-0089).
    } else if (entry == Phase::Maintenance) {
        // Serve the results dashboard, never returns. `panel` is the null fallback if init failed, so
        // the Maintenance screen no-ops on a dead panel while the SoftAP still serves (ADR-0045).
        runMaintenance(creds, panel);  // reached via the runtime Maintenance-key reboot (ADR-0057).
    } else {
        // hunt_loop copies the creds it needs; this local may go out of scope. Pass the panel only if it
        // initialised — nullptr reuses hunt_loop's proven "[SCREEN] disabled" path (ADR-0045 decision 3).
        runStationBoot(creds, panelReady ? &display() : nullptr);
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
    if (rssiProbeActive()) {  // RSSI-correlation probe owns the device; the normal boot loop is skipped.
        rssiProbePump();
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
    if (keyboardProbeActive()) {  // bench keyboard-keycode probe owns the device; normal boot is skipped.
        keyboardProbePump();
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
        // Wake the inactivity dimmer on a board input (ADR-0093 decision 3). Button boards poll the entry
        // button: any press (including the start of a Maintenance hold) counts as activity. Keyboard-board
        // wake is deferred (LEDGER) because maintenanceEntryRequested() already drains the key FIFO for `m`,
        // so a parallel any-key poll would contend for it; a capture/crack bus fact still wakes every board.
        if constexpr (kActiveBoard.input == InputKind::Buttons) {
            if (kActiveBoard.bootButtonPin >= 0 && digitalRead(kActiveBoard.bootButtonPin) == LOW) {
                huntLoopNoteInput(millis());
            }
        }
        // Runtime Maintenance entry (ADR-0057 intent path, ADR-0078 per-board gesture): the board's entry
        // gesture (keyboard `m`-press or button hold) persists the reboot-intent and restarts; the clean
        // boot reads the intent and routes to Maintenance via the boot gate — never a live radio swap
        // (§4 #21). The poll is cheap (an ISR flag on keyboard boards; a pin read on button boards).
        if (maintenanceEntryRequested()) {
            Serial.println("[MAINT] Maintenance gesture — rebooting into Maintenance");
            g_maintenanceIntentWord = kMaintenanceIntentMagic;
            delay(50);  // let the line flush before the reset.
            ESP.restart();
        }
    }
    delay(5);
}
