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

#include "net/cracked_sync.h"  // SyncOutcome, for the sync getters below.
#include "net/provisioning_store.h"
#include "net/upload_supervisor.h"

namespace sapper {

/// Build and start the hunt/upload spine for @p creds. Returns false (loud) if the capture store
/// could not mount or the sniffer could not enter promiscuous mode. @p config tunes the drain
/// arbitration; the shipped caller takes the defaults, the verify uses a snappier profile.
bool huntLoopBegin(const ProvisioningRecord& creds, const UploadSupervisorConfig& config = {});

/// Advance the hunt and the drain arbiter once. Call every loop() iteration after huntLoopBegin().
void huntLoopPump();

/// The most recent drain outcome, for a device surface / the on-air verify to report.
const DrainOutcome& huntLoopLastDrain();

/// Monotonic count of drain cycles run, so an observer can detect each new drain.
uint32_t huntLoopDrainCount();

/// The most recent cracked-results sync outcome, for a device surface / the on-air verify to report.
const SyncOutcome& huntLoopLastSync();

/// Monotonic count of syncs whose outcome was reported, so an observer can detect each new one.
uint32_t huntLoopSyncCount();

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

}  // namespace sapper

#endif  // UNIT_TEST
