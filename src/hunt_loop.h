/**
 * @file hunt_loop.h
 * @brief The shipped hunt→enqueue→drain→upload loop: the appliance's steady state (ADR-0017
 *        decision #8). Device-only.
 *
 * This is the first shipped code that runs the endless AutoHunt for real and does something with each
 * capture — the loop ADR-0015 decision #8 could not ship because there was no uploader. It owns the
 * real capture/upload spine (Esp32RadioSniffer → HuntEngine → CaptureQueue over LittleFS → wpa-sec
 * Uploader, arbitrated by the UploadSupervisor) as file-scope singletons, begun once the device is
 * provisioned and past the boot gate (ADR-0006), and pumped every loop() iteration.
 *
 * It is not gated by SAPPER_TEST_HOOKS: the shipped appliance hunts and uploads. The only test-hooks
 * addition is a stimulus injector the on-air verify uses to exercise the upload path without waiting
 * for a natural (deauth-forced) capture — kept behind SAPPER_TEST_HOOKS so no shipped binary carries
 * the injection vocabulary (§4 invariant #4).
 */
#pragma once

#ifndef UNIT_TEST

#include "hal/display/display_hal.h"  // IDisplay — the optional panel the status HUD renders on.
#include "net/cracked_sync.h"  // SyncOutcome, for the sync getters below.
#include "net/hunt_snapshot.h"  // HuntSnapshot — the live-hunt observation getter below (ADR-0033).
#include "net/provisioning_store.h"
#include "net/upload_supervisor.h"

namespace sapper {

/// Build and start the hunt/upload spine for @p creds. Returns false (loud) if the capture store
/// could not mount or the sniffer could not enter promiscuous mode. @p config tunes the drain
/// arbitration; the shipped caller takes the defaults, the verify uses a snappier profile. @p display
/// is optional (ADR-0025): when non-null and the active board has a panel, the status-HUD/toast
/// surface is wired onto it; a null display (the LED/webhook/upload/sync probes) leaves the screen off.
bool huntLoopBegin(const ProvisioningRecord& creds, const UploadSupervisorConfig& config = {},
                   IDisplay* display = nullptr);

/// Advance the hunt and the drain arbiter once. Call every loop() iteration after huntLoopBegin().
void huntLoopPump();

/// The most recent drain outcome, for a device surface / the on-air verify to report.
const DrainOutcome& huntLoopLastDrain();

/// Monotonic count of drain cycles run, so an observer can detect each new drain.
uint32_t huntLoopDrainCount();

/// The engine's live pull snapshot (ADR-0033): current phase/channel/target and collected-message set.
/// For a probe/verify to observe the live HUD state; a zeroed snapshot before the loop begins.
HuntSnapshot huntLoopSnapshot();

/// The most recent cracked-results sync outcome, for a device surface / the on-air verify to report.
const SyncOutcome& huntLoopLastSync();

/// Monotonic count of syncs whose outcome was reported, so an observer can detect each new one.
uint32_t huntLoopSyncCount();

/// Monotonic count of webhook notifications successfully POSTed (ADR-0023), so the on-air webhook
/// verify can detect each send. Zero when no webhook URL is provisioned (the surface is disabled).
uint32_t huntLoopWebhookSentCount();

#ifdef SAPPER_TEST_HOOKS
/// Re-arm the hourly scheduler so a cracked-results sync is due on the next STA window — the
/// clock-advance stimulus the on-air sync verify uses instead of waiting a real hour (slice-0020
/// Scenario J). Present only in test-hooks builds (§4 invariant #4).
void huntLoopForceSyncDue();
#endif

#ifdef SAPPER_TEST_HOOKS
/// Inject one synthetic wpa-sec-valid handshake through the capture-ready seam, exactly as the engine
/// would on a real capture (§4 invariant #2) — the on-air verify's stimulus. Present only in
/// test-hooks builds (§4 invariant #4).
void huntLoopInjectStimulus();
#endif

#ifdef SAPPER_TEST_HOOKS
/// Push the cracked-sync deadline out by a full interval (as a successful sync would), so no sync-due
/// STA window opens and stops the engine. The hunt-HUD verify calls it so the engine can hunt
/// uninterrupted on a network-less bench. Present only in test-hooks builds (§4 invariant #4).
void huntLoopDeferSync();
#endif

#ifdef SAPPER_TEST_HOOKS
/// Inject a synthetic beacon + M1 (a PARTIAL handshake, no M2) through the engine's `onFrame` — the SAME
/// seam a real sniffed frame uses (§4 invariant #2), never a parallel path — so the engine discovers,
/// captures, and *populates* its collector, lighting the live hunt HUD's Beacon + M1 indicators. Partial
/// on purpose: a complete (wpa-sec-valid) set would make the engine report-and-advance, so the HUD would
/// not persist for a `dump`. The hunt-HUD verify's stimulus (ADR-0033). Test-hooks builds only (§4 #4).
void huntLoopInjectHudStimulus();
#endif

#ifdef SAPPER_TEST_HOOKS
/// Publish one synthetic new-password fact onto the event bus, exactly as a real steady-state crack
/// would (§4 invariant #2: the bus is the same seam the sync publishes through) — the LED verify's
/// stimulus for the "recovered" flash, which a real crack cannot force on demand. Present only in
/// test-hooks builds (§4 invariant #4).
void huntLoopInjectCrackedAlert();

/// Publish one synthetic HandshakeCaptured fact onto the event bus, exactly as the supervisor does after
/// a real enqueue (§4 invariant #2) — the webhook-capture verify's stimulus for the capture push
/// (ADR-0035), which a real capture cannot force on demand. Identity-only (§4 #17). Present only in
/// test-hooks builds (§4 invariant #4).
void huntLoopInjectCaptureAlert();
#endif

}  // namespace sapper

#endif  // UNIT_TEST
