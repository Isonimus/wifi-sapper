/**
 * @file screen_toast_surface.h
 * @brief The status-HUD + cracked-toast display surface (ADR-0025).
 *
 * The third slice-7 surface and the second passive one (after the LED). It subscribes to the event
 * bus (ADR-0021), keeps a small model of "what is the appliance doing and what has it found", and
 * drives an abstract ScreenRenderer — so the whole policy (which fact updates which HUD field, when
 * the CRACKED banner arms and clears, and the liveness heartbeat) is pure and host-tested against a
 * fake renderer. It owns no panel and no clock: time enters only through tick(now), exactly like the
 * LED surface and the engine, so the toast hold and the heartbeat cadence are asserted on the native
 * lane with a fake clock.
 *
 * The design mirrors LedStatusSurface deliberately: onAppEvent only updates internal state (it has no
 * clock); tick(now) does every clock-based decision and is the only thing that renders — and it
 * renders only when the computed view differs from the last one shown, so the panel sees discrete
 * updates, not a per-tick redraw stream.
 */
#pragma once

#include <cstdint>

#include "core/event_bus.h"
#include "surface/screen_renderer.h"
#include "surface/screen_view.h"

namespace sapper {

class ScreenToastSurface : public EventSink {
public:
    explicit ScreenToastSurface(ScreenRenderer& renderer) : renderer_(renderer) {}

    /// Seed the clock anchor and render the initial HUD. Call once after the engine is running, before
    /// the first tick().
    void begin(uint32_t nowMs);

    /// Bus subscription: update the model (status, counters, arm the toast) from one fact. Never reads
    /// a clock or renders — tick() does (onAppEvent has no clock, like the LED surface).
    void onAppEvent(const AppEvent& event) override;

    /// Advance the toast expiry and the heartbeat, then render the current view if it changed. Call
    /// every app-loop iteration after the supervisor's tick, so a fact published this iteration shows
    /// the same iteration.
    void tick(uint32_t nowMs);

private:
    /// How long a freshly recovered password holds the CRACKED banner before it clears back to the HUD.
    static constexpr uint32_t kToastHoldMs = 6000;
    /// How long a freshly captured handshake holds the CAPTURED banner. Shorter than the crack banner:
    /// captures are frequent, so a long hold would leave the banner up near-continuously (ADR-0031 #4).
    static constexpr uint32_t kCapturedHoldMs = 2500;
    /// Half the heartbeat period: the liveness pulse toggles every second (a 2 s blink), slow enough to
    /// read as "alive", fast enough to notice it stop — the reason a status LED blinks (slice-0022).
    static constexpr uint32_t kHeartbeatHalfPeriodMs = 1000;

    /// The resting status a completed drain implies — the same reading LedStatusSurface uses (ADR-0025
    /// decision 4). Duplicated rather than shared: this is the second reader, not the third (LEDGER).
    static ScreenStatus statusFromDrain(const DrainOutcome& outcome);

    /// Whether the heartbeat is in its lit half at @p nowMs (wrap-safe across the millis() rollover).
    bool heartbeatOn(uint32_t nowMs) const;

    /// Compose the current ScreenView from the model + clock, and render it only if it changed.
    void render(uint32_t nowMs);

    ScreenRenderer& renderer_;
    ScreenStatus status_ = ScreenStatus::Booting;  ///< The base HUD status the drain facts set.
    uint32_t uploaded_ = 0;                        ///< Cumulative accepted + duplicate uploads.
    uint32_t cracks_ = 0;                          ///< Cumulative NewPassword facts.
    bool haveSynced_ = false;
    bool lastSyncOk_ = false;
    uint32_t lastSyncNew_ = 0;
    uint32_t heartbeatAnchorMs_ = 0;               ///< When begin() ran; the pulse phase is measured hence.
    bool toastPending_ = false;                    ///< A fact armed a banner; the next tick starts its hold.
    bool toastActive_ = false;                     ///< A banner (CAPTURED or CRACKED) is currently shown.
    ToastKind pendingKind_ = ToastKind::Cracked;   ///< Which banner the pending arm will show.
    ToastKind activeKind_ = ToastKind::Cracked;    ///< Which banner is currently shown (read when active).
    uint32_t toastUntilMs_ = 0;                    ///< When the banner clears back to the HUD.
    char toastEssid_[kCrackedEssidCap] = {0};      ///< The pending/active banner's ESSID (printable-filtered).
    char toastBssid_[kBssidTextCap] = {0};         ///< Its formatted BSSID.
    ScreenView lastShown_;                         ///< What the renderer was last given, for edge-only render.
    bool begun_ = false;
};

}  // namespace sapper
