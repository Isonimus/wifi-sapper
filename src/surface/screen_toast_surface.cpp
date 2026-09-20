/**
 * @file screen_toast_surface.cpp
 * @brief Implementation of the status-HUD + cracked-toast display surface (ADR-0025).
 */
#include "surface/screen_toast_surface.h"

#include <cstdio>
#include <cstring>

#include "core/deadline.h"          // reached(): wrap-safe latch/heartbeat timing.
#include "net/cracked_sync.h"       // SyncOutcome — the SyncCompleted payload.
#include "net/upload_supervisor.h"  // DrainOutcome — the DrainCompleted payload statusFromDrain reads.

namespace sapper {
namespace {

/// Copy @p src into @p dst (bounded, always NUL-terminated), replacing any non-printable byte (a
/// control byte < 0x20 or a DEL) with '?'. An 802.11 SSID is arbitrary octets; a control byte in the
/// panel text could scramble the line or move the cursor. This is a lighter guard than the webhook's
/// full UTF-8 sanitiser (a garbled glyph is cosmetic; a rejected JSON body wedged the retry queue), so
/// the two are deliberately not shared yet — the webhook is the first user, this the second (LEDGER).
void copyPrintable(char* dst, size_t dstCap, const char* src) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dstCap; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        dst[i] = (c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c);
    }
    dst[i] = '\0';
}

void formatBssid(const uint8_t bssid[6], char (&out)[kBssidTextCap]) {
    std::snprintf(out, sizeof(out), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2],
                  bssid[3], bssid[4], bssid[5]);
}

}  // namespace

void ScreenToastSurface::begin(uint32_t nowMs) {
    status_ = ScreenStatus::Hunting;  // the engine is running by the time begin() is called.
    heartbeatAnchorMs_ = nowMs;
    toastPending_ = false;
    toastActive_ = false;
    begun_ = true;
    lastShown_ = ScreenView{};            // force the first render() to differ and draw the HUD at boot.
    lastShown_.status = ScreenStatus::Fault;  // a value the fresh HUD (Hunting) cannot equal.
    render(nowMs);
}

void ScreenToastSurface::onAppEvent(const AppEvent& event) {
    switch (event.type) {
        case AppEventType::DrainStarted:
            status_ = ScreenStatus::Working;  // off-air: associating / uploading / syncing.
            break;
        case AppEventType::DrainCompleted: {
            const DrainOutcome& d = *event.drain;
            status_ = statusFromDrain(d);
            uploaded_ += d.accepted + d.duplicate;  // terminal upload successes (ADR-0025 decision 4).
            break;
        }
        case AppEventType::NewPassword: {
            const CrackedResult& r = *event.password;
            ++cracks_;
            // Copy the ESSID + BSSID only — never r.password (there is no field on the view for it), so
            // the recovered PSK cannot reach the panel. The ESSID is printable-filtered so a control
            // byte in an arbitrary SSID cannot corrupt the drawn line.
            copyPrintable(toastEssid_, sizeof(toastEssid_), r.essid);
            formatBssid(r.bssid, toastBssid_);
            pendingKind_ = ToastKind::Cracked;  // a crack always wins the slot (ADR-0031 #4).
            toastPending_ = true;  // the next tick starts the banner hold (onAppEvent has no clock).
            break;
        }
        case AppEventType::HandshakeCaptured: {
            // A crack outranks a capture: never overwrite a CRACKED banner that is showing or already
            // pending (ADR-0031 #4). Otherwise raise a CAPTURED banner naming the just-captured network.
            const bool crackedHoldsSlot = (toastActive_ && activeKind_ == ToastKind::Cracked) ||
                                          (toastPending_ && pendingKind_ == ToastKind::Cracked);
            if (crackedHoldsSlot) break;
            const CaptureFact& c = *event.capture;
            copyPrintable(toastEssid_, sizeof(toastEssid_), c.ssid);  // empty for a hidden network.
            formatBssid(c.bssid, toastBssid_);
            pendingKind_ = ToastKind::Captured;
            toastPending_ = true;
            break;
        }
        case AppEventType::SyncCompleted: {
            const SyncOutcome& o = *event.sync;
            haveSynced_ = true;
            lastSyncOk_ = o.ok;
            lastSyncNew_ = o.newPasswords;
            break;
        }
        case AppEventType::FirstSyncSummary:
            // A fresh manifest seeds silently (ADR-0019 decision #5): the backlog is not "cracked now",
            // so it raises no banner. The HUD's sync line is set by the real SyncCompleted that follows.
            break;
    }
}

void ScreenToastSurface::tick(uint32_t nowMs) {
    if (!begun_) return;
    if (toastPending_) {
        toastPending_ = false;
        toastActive_ = true;
        activeKind_ = pendingKind_;
        // A crack banner holds longer than a capture banner (ADR-0031 #4). (Re)arm from now, extending
        // the hold on a repeat of the same kind.
        toastUntilMs_ = nowMs + (activeKind_ == ToastKind::Cracked ? kToastHoldMs : kCapturedHoldMs);
    }
    if (toastActive_ && reached(nowMs, toastUntilMs_)) toastActive_ = false;
    render(nowMs);
}

ScreenStatus ScreenToastSurface::statusFromDrain(const DrainOutcome& outcome) {
    if (outcome.resumeFailed) return ScreenStatus::Fault;  // could not re-enter promiscuous — hard fault.
    // Degraded is the actionable connectivity signal (the last cycle could not associate); per-capture
    // upload outcomes are routine and shown as counters, not as a status, exactly as the LED scopes it
    // (slice-0022 Scenario J finding).
    return outcome.associated ? ScreenStatus::Hunting : ScreenStatus::Degraded;
}

bool ScreenToastSurface::heartbeatOn(uint32_t nowMs) const {
    // uint32 subtraction gives the correct elapsed time across the millis() wrap; the flag alternates
    // every half-period.
    return ((nowMs - heartbeatAnchorMs_) / kHeartbeatHalfPeriodMs) % 2 == 0;
}

void ScreenToastSurface::render(uint32_t nowMs) {
    ScreenView view;
    view.status = status_;
    view.uploaded = uploaded_;
    view.cracks = cracks_;
    view.haveSynced = haveSynced_;
    view.lastSyncOk = lastSyncOk_;
    view.lastSyncNew = lastSyncNew_;
    view.heartbeat = heartbeatOn(nowMs);
    view.toastActive = toastActive_;
    view.toastKind = activeKind_;
    if (toastActive_) {
        std::snprintf(view.toastEssid, sizeof(view.toastEssid), "%s", toastEssid_);
        std::snprintf(view.toastBssid, sizeof(view.toastBssid), "%s", toastBssid_);
    }
    if (view != lastShown_) {
        renderer_.render(view);
        lastShown_ = view;
    }
}

}  // namespace sapper
