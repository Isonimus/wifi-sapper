/**
 * @file hunt_engine.h
 * @brief The headless endless-AutoHunt state machine over the capture seams (ADR-0015).
 *
 * Orchestrates the existing pure pieces — a RadioSniffer (the only hardware seam), a ChannelHopper,
 * an ApRegistry, and a HandshakeCollector — into the loop the appliance is: Discovering (sweep the
 * band, enumerate APs) → Capturing (round-robin each discovered AP, collect its handshake) →
 * Discovering, forever. On a wpa-sec-valid handshake it raises a capture-ready event and moves on; it
 * never uploads, connects as a station, or drives a display (headless — ADR-0001; slice-5/7 subscribe
 * to the event).
 *
 * The engine is itself the sniffer's single, stable FrameConsumer for the whole run (ADR-0015
 * decision #1). onFrame runs in the Wi-Fi-driver task and does nothing but load one atomic pointer and
 * forward the frame to the sink the current phase uses (decision #2) — the consumer is never swapped
 * under the running radio (which stop()/begin() cannot do safely, ADR-0011; §4 invariant #11). Every
 * transition that clears a sink passes through a non-blocking quiesce settle — the router aimed away,
 * a bounded wait on the caller's clock — so no sink is reset while the driver task may still be
 * writing it (decision #4; discharges the two cross-task audit items ADR-0011 and ADR-0013 parked
 * here). All state-machine work happens in tick(now); the radio never reaches the logic, so the whole
 * loop is host-tested through an injected fake sniffer and a fake clock (ADR-0004 lane 1).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "net/ap_registry.h"
#include "net/channel_hopper.h"
#include "net/handshake_collector.h"
#include "net/handshake_consumer.h"
#include "net/radio_sniffer.h"

namespace sapper {

/// Notified of each wpa-sec-valid handshake the engine captures. Implemented off the engine (the
/// uploader, slice-5; the alerter, slice-7; a display) — the headless event boundary (ADR-0001). The
/// engine calls it from tick() on the app task, never in the driver callback, so the subscriber may do
/// heavy work (ADR-0015 decision #6). The handshake reference is valid only for the call.
class CaptureReadyObserver {
public:
    virtual ~CaptureReadyObserver() = default;
    virtual void onCaptureReady(const CapturedHandshake& handshake) = 0;
};

/// Loop tuning. The defaults are architecture-derived; the settle floor is a hardware measurement
/// (ADR-0015 decision #4, LEDGER), and the dwell floor is ADR-0013 decision #5's.
struct HuntConfig {
    uint32_t dwellMs = 300;            ///< Per-channel dwell during discovery (ADR-0013 decision #5).
    uint32_t discoverWindowMs = 4000;  ///< How long one discovery sweep runs before selecting targets.
    uint32_t captureWindowMs = 8000;   ///< How long to sit on one target before advancing.
    uint32_t settleMs = 20;            ///< Quiesce settle before a sink is reset/re-targeted.
};

/**
 * @brief Drives the endless discover→capture→discover loop over the capture seams.
 *
 * Construct with the seam, the sinks it owns/uses, an observer, and the hop channel list; call
 * begin(now) once, then tick(now) every app-loop iteration with a rising clock. The engine owns the
 * HandshakeCollector (re-targeted across the round-robin) and drives the caller-supplied ApRegistry
 * and RadioSniffer. The caller must not destroy the engine or its sinks within one settleMs of stop()
 * — the driver callback cannot be hard-joined (ADR-0011).
 */
class HuntEngine : public FrameConsumer {
public:
    enum class Phase { Idle, Discovering, Capturing, Quiescing };

    HuntEngine(RadioSniffer& sniffer, ApRegistry& registry, CaptureReadyObserver& observer,
               const uint8_t* channels, size_t channelCount, const HuntConfig& config = {});

    /// Install the engine as the sniffer's consumer on the first hop channel and enter Discovering.
    /// Returns false (and stays Idle) if the sniffer could not enter promiscuous mode.
    bool begin(uint32_t nowMs);

    /// Advance the state machine. Idempotent when Idle.
    void tick(uint32_t nowMs);

    /// Aim the router away and leave promiscuous mode. The caller stops ticking after this.
    void stop();

    /// Driver-task context: forward one raw frame to the active sink, nothing else (ADR-0015 #2).
    void onFrame(const uint8_t* frame, uint16_t len) override;

    // --- Observation (app task), for the probe and the host tests. ---
    Phase phase() const { return phase_; }
    uint8_t parkedChannel() const { return parkedChannel_; }
    size_t discoveredCount() const { return registry_.count(); }
    /// The BSSID being captured, or nullptr when not in Capturing.
    const uint8_t* capturingBssid() const;

private:
    /// Post-settle action a Quiescing phase completes into.
    enum class Resume { Discovering, CaptureFromIndex, ReportThenAdvance, AdvanceNoReport };

    void enterQuiesce(uint32_t nowMs, Resume resume);
    void doEnterDiscovering(uint32_t nowMs);
    /// Try to start capturing from targetIndex_, skipping any target whose channel the radio rejects;
    /// falls back to discovery if none remain (ADR-0015 decision #5).
    void doStartCapturing(uint32_t nowMs);
    void advanceTarget(uint32_t nowMs);
    /// Retune the radio to @p channel and, only on success, record it as the parked channel and the
    /// registry's fallback (ADR-0013 decision #3). Returns whether the radio accepted the channel.
    bool retuneTo(uint8_t channel);

    RadioSniffer& sniffer_;
    ApRegistry& registry_;
    CaptureReadyObserver& observer_;
    HandshakeCollector collector_;         // owned; re-targeted across the round-robin.
    HandshakeConsumer handshakeConsumer_;  // wraps collector_ — must follow it in declaration order.
    ChannelHopper hopper_;
    HuntConfig config_;

    std::atomic<FrameConsumer*> activeSink_{nullptr};  // the one field onFrame and tick share.
    Phase phase_ = Phase::Idle;
    Resume resume_ = Resume::Discovering;
    uint32_t phaseDeadlineMs_ = 0;  // Discovering/Capturing window end.
    uint32_t quiesceUntilMs_ = 0;   // Quiescing settle end.
    size_t targetCount_ = 0;        // snapshot of registry.count() at discovery's end.
    size_t targetIndex_ = 0;        // round-robin cursor into the snapshot.
    uint8_t parkedChannel_ = 0;     // the channel the radio last accepted.
};

}  // namespace sapper
