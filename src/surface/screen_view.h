/**
 * @file screen_view.h
 * @brief The pure view-model the screen surface produces and the renderer consumes (ADR-0025).
 *
 * ScreenView names WHAT the panel should show, never how: no pixel coordinates, fonts, or colours. The
 * pure ScreenToastSurface computes one of these from the bus facts + the clock, and pushes it to the
 * abstract ScreenRenderer only when it changes. The device renderer turns it into glyphs; a fake
 * renderer records it so the policy is host-tested on the model, not on draw calls (ADR-0025 decision
 * 2). It is a POD — no heap, no hardware — so it compiles and is asserted on the native lane.
 */
#pragma once

#include <cstdint>
#include <cstring>

#include "net/cracked_result.h"  // kCrackedEssidCap — the ESSID bound the toast copies within.

namespace sapper {

/// What the appliance is doing, for the HUD status line. Mirrors the LED's coarse status (ADR-0025
/// decision 4): Working while off-air in a drain, then the resting state the last cycle produced.
enum class ScreenStatus : uint8_t {
    Booting,   ///< Before the first drain — the initial state begin() shows.
    Hunting,   ///< Sniffing for handshakes (the healthy resting state).
    Working,   ///< Off-air in an STA window: associating, uploading, syncing.
    Degraded,  ///< The last cycle could not associate — connectivity is the actionable problem.
    Fault,     ///< The engine could not re-enter promiscuous mode after a drain — a hard fault.
};

/// A formatted BSSID is 17 chars ("AA:BB:CC:DD:EE:FF") plus a NUL.
constexpr size_t kBssidTextCap = 18;

/// Which banner the toast slot is showing, so the renderer picks the label (ADR-0031 decision 4). One
/// slot renders both: a freshly captured handshake (CAPTURED) or a freshly recovered password
/// (CRACKED). Only meaningful while ScreenView::toastActive is true.
enum class ToastKind : uint8_t {
    Captured,  ///< A handshake was just enqueued for upload (frequent; a brief banner).
    Cracked,   ///< A password was just recovered (rare; the bigger news, outranks a capture).
};

/**
 * @brief The complete state of the panel at one moment. Compared field-by-field for render-on-change.
 *
 * All counters are cumulative over the device's run and derived only from bus facts (invariant #13):
 * no capture-queue depth appears, because the bus carries none. The toast carries the ESSID and BSSID
 * of a freshly cracked network and, by construction, NO password field — the recovered PSK cannot
 * reach the panel (ADR-0025 decision 4; mirrors ADR-0023's structural no-PSK guarantee).
 */
struct ScreenView {
    ScreenStatus status = ScreenStatus::Booting;
    uint32_t uploaded = 0;      ///< Cumulative accepted + duplicate uploads (terminal successes).
    uint32_t cracks = 0;        ///< Cumulative NewPassword facts seen.
    bool haveSynced = false;    ///< At least one SyncCompleted fact has arrived.
    bool lastSyncOk = false;    ///< The last sync succeeded.
    uint32_t lastSyncNew = 0;   ///< New passwords the last sync announced.
    bool heartbeat = false;     ///< Liveness pulse; alternates ~1 Hz so a live HUD is not a frozen one.
    bool toastActive = false;   ///< A banner (CAPTURED or CRACKED) is currently shown over the HUD.
    ToastKind toastKind = ToastKind::Cracked;  ///< Which banner is shown; read only when toastActive.
    char toastEssid[kCrackedEssidCap] = {0};  ///< The banner network's name (printable-filtered).
    char toastBssid[kBssidTextCap] = {0};     ///< Its BSSID, formatted "AA:BB:CC:DD:EE:FF".

    /// Field-by-field equality: the surface renders only when the displayed state actually changes, so
    /// the panel sees discrete updates rather than a per-tick redraw (ADR-0025 decision 2).
    bool operator==(const ScreenView& o) const {
        return status == o.status && uploaded == o.uploaded && cracks == o.cracks &&
               haveSynced == o.haveSynced && lastSyncOk == o.lastSyncOk &&
               lastSyncNew == o.lastSyncNew && heartbeat == o.heartbeat &&
               toastActive == o.toastActive && toastKind == o.toastKind &&
               std::strcmp(toastEssid, o.toastEssid) == 0 &&
               std::strcmp(toastBssid, o.toastBssid) == 0;
    }
    bool operator!=(const ScreenView& o) const { return !(*this == o); }
};

}  // namespace sapper
