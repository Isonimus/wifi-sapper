/**
 * @file window_notifier.h
 * @brief The window-sharer seam: something the supervisor runs while the STA window is open (ADR-0023).
 *
 * A *transmitting* surface (the push webhook, and any later SMS/email/relay) observes facts on the
 * EventBus (ADR-0021) but can only transmit when the appliance has connectivity — which exists only
 * inside a supervisor-owned STA window (ADR-0023, forced by upload_supervisor.cpp). This one-method
 * seam is how the UploadSupervisor offers that window to such a surface: it calls flushInWindow() after
 * the drain and the cracked-results sync, before tearing the station down, so a fact enqueued this
 * window (or one left unsent from a previous window) is transmitted while the radio is associated.
 *
 * It is the same relationship the supervisor already has with the SyncSession (ADR-0019 decision #6): a
 * window-sharer the supervisor drives inside the window. It lives in net/, below surface/, so the
 * supervisor depends on this abstraction and never on the surface layer (dependency inversion) — the
 * webhook (surface/webhook_notifier.h) implements it.
 */
#pragma once

namespace sapper {

/// A collaborator the supervisor invokes while an STA window is open. The single call must be
/// self-contained: transmit whatever is pending and return; a failure is the implementation's to
/// retain and retry on a later window (the supervisor does not inspect the result).
class WindowNotifier {
public:
    virtual ~WindowNotifier() = default;

    /// Called once per drain cycle, station up, after the sync and before tear-down. Transmit any
    /// pending notifications now; keep the ones that fail for the next window (ADR-0023 decision 2).
    virtual void flushInWindow() = 0;
};

}  // namespace sapper
