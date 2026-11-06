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
#include "net/deauth.h"  // ManagementSubtype, for the armed-deauth burst (ADR-0029).
#include "net/handshake_collector.h"
#include "net/pmkid_solicit.h"  // buildAuthRequest/buildAssocRequest, for the solicit injector (ADR-0110).
#include "net/handshake_consumer.h"
#include "net/hunt_snapshot.h"  // HuntPhase (aliased as Phase), HuntSnapshot, HuntSnapshotSource (ADR-0033).
#include "net/radio_sniffer.h"
#include "net/raw_transmitter.h"

namespace sapper {

struct CrackedLookup;    // net/cracked_lookup.h — the read-only isCracked(bssid) seam (ADR-0059/-0061 gate B).
struct SubmittedLookup;  // net/submitted_lookup.h — the read-only isSubmitted(bssid) seam (ADR-0065 gate C).

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
    /// Cadence of the broadcast deauth burst during Capturing when a transmitter is injected
    /// (ADR-0029). Deliberately coarse: a deauthed client re-associates on a seconds scale and the
    /// 4-way handshake we want only completes *after* it does, so this leaves a listen gap rather
    /// than deauthing continuously. Architecture-derived, not yet a measurement (LEDGER, like dwell).
    uint32_t deauthIntervalMs = 2000;
    /// Cadence of the clientless-PMKID solicit burst during Capturing when the solicit arm is set and a
    /// transmitter is present (ADR-0110 #2). Each burst is the blind pair (open-auth, then the association
    /// request an inter-frame gap later). Carried from the sibling's measured as-built ~1500 ms
    /// (adversary:ADR-0138); architecture-derived against our capture window, tuning deferred (LEDGER).
    uint32_t solicitIntervalMs = 1500;
    /// Floor on the gap between the open-auth and the association request of one solicit burst (ADR-0110
    /// #2). Realised non-blocking: the auth goes out on the due tick and the assoc on a later tick once
    /// this has elapsed, so the real gap is this value plus at most one app-loop tick (the safe direction;
    /// the AP holds the open-auth state well beyond it). The sibling's proven 25 ms (adversary:ADR-0138).
    uint32_t solicitInterFrameMs = 25;
};

/// The RSSI partition floor for target ordering (ADR-0068 #31): an AP at or above it is attempted before
/// any below it. -80 dBm is the measured knee from the `field-2026-10-01` run
/// (artifacts/0068-field-2026-10-01.txt) — capture rate was 48% in the -70..-80 band and 2% below -80.
/// A constant, not a HuntConfig field: it is a measured default with no present need to vary per instance
/// (KISS); a future tuning pass would supersede this value, not make it runtime-configurable.
constexpr int8_t kRssiFloor = -80;

/// Never-captured-AP backoff (ADR-0070 #32): an AP that yields no wpa-sec-valid handshake in this many
/// consecutive capture windows is treated as client-less/PMF (demonstrated unproductivity) and skipped in
/// target selection for a cadence — reclaiming the windows the RSSI floor cannot (a strong client-less AP
/// sits in the floor's first-attempted partition, so #31 cannot help it). Architecture-derived over the
/// `field-2026-10-01` run, not a measurement (LEDGER tuning), like dwell/settle/deauth-cadence.
constexpr uint16_t kBackoffMissThreshold = 3;
/// Enumeration cycles a backed-off AP is suppressed before one re-attempt (ADR-0070 #32). Deprioritise for
/// a cadence, never permanently skip: a client that associates later is still caught within this bound, and
/// a still-empty retry re-suppresses it (one window per cadence, not a renewed N-window run).
constexpr uint32_t kBackoffCadenceCycles = 5;
/// The most APs the backoff table tracks at once — one discovery sweep's worth (ADR-0070 #32). Beyond it
/// the least-penalised entry is evicted; a mis-eviction only drops a backoff (the AP is re-attempted),
/// never a missed capture, so the bound fails toward attempting.
constexpr size_t kMaxBackoffEntries = kMaxDiscoveredAps;

/**
 * @brief Drives the endless discover→capture→discover loop over the capture seams.
 *
 * Construct with the seam, the sinks it owns/uses, an observer, and the hop channel list; call
 * begin(now) once, then tick(now) every app-loop iteration with a rising clock. The engine owns the
 * HandshakeCollector (re-targeted across the round-robin) and drives the caller-supplied ApRegistry
 * and RadioSniffer. The caller must not destroy the engine or its sinks within one settleMs of stop()
 * — the driver callback cannot be hard-joined (ADR-0011).
 */
class HuntEngine : public FrameConsumer, public HuntSnapshotSource {
public:
    /// The loop phase. Aliased to the shared HuntPhase (ADR-0033) so `HuntEngine::Phase::X` still names
    /// it while the pull-snapshot POD header can name the enum without including the engine.
    using Phase = HuntPhase;

    /// @param transmitter Optional raw-TX seam (ADR-0029). A non-null transmitter means only that the
    ///        raw-TX path is *available*; it no longer alone arms anything (ADR-0110 #5, the §4 #16
    ///        refinement). What is transmitted is gated by two independent default-off arms,
    ///        setDeauthArmed / setSolicitArmed: deauth needs a transmitter AND the deauth arm, solicit a
    ///        transmitter AND the solicit arm. `nullptr` (the default) is a purely passive hunt — the
    ///        disarmed state a shipped appliance holds until the operator arms it (invariant #16). The
    ///        engine still *builds* frames only through the pure `buildDeauthFrame`/`buildAuthRequest`/
    ///        `buildAssocRequest`.
    HuntEngine(RadioSniffer& sniffer, ApRegistry& registry, CaptureReadyObserver& observer,
               const uint8_t* channels, size_t channelCount, const HuntConfig& config = {},
               RawTransmitter* transmitter = nullptr);

    /// Arm gate B (ADR-0061, §4 #28): consult @p lookup in target selection and skip any discovered AP
    /// it reports already cracked — never tuning to, capturing, or (so, never) deauthing it. Off by
    /// default: an unset lookup (`nullptr`) is the purely-frame-fed engine of §4 #8/#10, targeting every
    /// discovered AP. The lookup is read only on the app task (target selection), never in onFrame, so
    /// it adds no persisted-state read to the frame path. Mirrors UploadSupervisor::setCrackedLookup
    /// (gate A) and the optional RawTransmitter arm.
    void setCrackedLookup(const CrackedLookup& lookup) { crackedLookup_ = &lookup; }

    /// Arm gate C (ADR-0065, §4 #30): consult @p lookup in target selection and skip any discovered AP
    /// it reports already submitted to wpa-sec — the general form of gate B (submitted ⊇ cracked), so it
    /// skips a network from the moment a valid handshake landed, not merely once it is cracked. Off by
    /// default: an unset lookup (`nullptr`) leaves the engine unchanged. Read only on the app task
    /// (target selection), never in onFrame, so §4 #8/#10 hold as with gate B. Complements — does not
    /// replace — setCrackedLookup: a target is skipped if cracked OR submitted, keeping the cracked
    /// floor authoritative even after a local submitted-set loss (ADR-0065 #4).
    void setSubmittedLookup(const SubmittedLookup& lookup) { submittedLookup_ = &lookup; }

    /// Arm deauth (ADR-0029, §4 #16): during Capturing, broadcast deauth/disassoc at the current target
    /// to force reassociation. Off by default; takes effect only when a transmitter was also injected.
    /// Decoupled from the transmitter's presence by ADR-0110 #5 (the §4 #16 refinement) so a device can
    /// be solicit-armed with deauth off. App task only; the engine still builds frames only through the
    /// pure `buildDeauthFrame`.
    void setDeauthArmed(bool armed) { deauthArmed_ = armed; }

    /// Arm clientless-PMKID solicitation (ADR-0110 #2/#5): during Capturing, inject the blind pair
    /// (open-auth, then an association request with a CCMP+PSK RSN IE) on the solicit cadence to draw an
    /// M1 with a PMKID KDE from a PMK-caching AP with no client present. Off by default; takes effect only
    /// when a transmitter was also injected. Independent of the deauth arm — the four combinations give
    /// the sibling's passive / Deauth / PMKID / Both strategy (ADR-0110 #5). App task only; frames are
    /// built only through the pure `buildAuthRequest`/`buildAssocRequest`.
    void setSolicitArmed(bool armed) { solicitArmed_ = armed; }

    /// Install the engine as the sniffer's consumer on the first hop channel and enter Discovering.
    /// Returns false (and stays Idle) if the sniffer could not enter promiscuous mode.
    bool begin(uint32_t nowMs);

    /// Advance the state machine. Idempotent when Idle.
    void tick(uint32_t nowMs);

    /// Aim the router away and leave promiscuous mode. The caller stops ticking after this.
    void stop();

    /// Driver-task context: forward one raw frame (and its @p rssi) to the active sink, nothing else
    /// (ADR-0015 #2). RSSI is receiver metadata passed straight through; the engine parses no protocol.
    void onFrame(const uint8_t* frame, uint16_t len, int8_t rssi) override;

    // --- Observation (app task), for the probe and the host tests. ---
    Phase phase() const { return phase_; }
    uint8_t parkedChannel() const { return parkedChannel_; }
    size_t discoveredCount() const { return registry_.count(); }
    /// The BSSID being captured, or nullptr when not in Capturing.
    const uint8_t* capturingBssid() const;
    /// Monotonic counts of deauth/disassoc frames the raw-TX seam accepted / rejected (ADR-0029), for
    /// a surface and the on-air verify. Both stay 0 on a disarmed (nullptr-transmitter) engine.
    uint32_t deauthTxOk() const { return deauthTxOk_; }
    uint32_t deauthTxFail() const { return deauthTxFail_; }
    /// Monotonic counts of solicit auth/assoc frames the raw-TX seam accepted / rejected (ADR-0110), for a
    /// surface and the on-air verify. Both stay 0 unless the solicit arm is set and a transmitter present.
    uint32_t solicitTxOk() const { return solicitTxOk_; }
    uint32_t solicitTxFail() const { return solicitTxFail_; }

    /// The live pull snapshot the HUD reads (ADR-0033, §4 #18): current phase, parked channel, discovered
    /// count, and the in-flight target's identity + which of Beacon/M1–M4 the collector holds. A
    /// best-effort, lock-free, app-task read of driver-task-written state — cosmetic by design (see the
    /// impl comment); never a bus event.
    HuntSnapshot huntSnapshot() const override;

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
    /// While Capturing and armed, transmit one broadcast deauth+disassoc burst at the current target
    /// once the cadence is due (ADR-0029 #1). A no-op when the transmitter is null (disarmed).
    void maybeTransmitDeauth(uint32_t nowMs);
    /// Build (pure) and transmit one broadcast frame of @p subtype at @p bssid; tally the outcome.
    void transmitDeauthFrame(ManagementSubtype subtype, const uint8_t bssid[6]);
    /// While Capturing and solicit-armed, drive the non-blocking two-phase solicit burst (ADR-0110 #2):
    /// on a due cadence transmit the open-auth and arm the assoc for an inter-frame gap later; on a later
    /// tick once that gap has elapsed transmit the association request and arm the next cadence. A no-op
    /// when the transmitter is null or the solicit arm is off. App task only (reached from tick()).
    void maybeTransmitSolicit(uint32_t nowMs);
    /// Build (pure) and transmit one solicit frame from @p frame of @p len bytes (0 = build failed, e.g.
    /// buffer too small); tally the outcome into the solicit counters. A build failure counts as a fail.
    void transmitSolicitFrame(const uint8_t* frame, uint16_t len);

    /// Record how the capture window that just ended for @p bssid resolved, for the never-captured backoff
    /// (ADR-0070 #32): @p captured true clears the AP's miss record (rehabilitated), false increments its
    /// consecutive-miss count and, at kBackoffMissThreshold, suppresses it for kBackoffCadenceCycles cycles.
    /// App task only (the Quiescing-resume handler), never onFrame — §4 #8/#10 hold.
    void registerCaptureOutcome(const uint8_t* bssid, bool captured);
    /// Whether @p bssid is currently in backoff (its suppression horizon is still ahead of the cycle
    /// counter), so target selection skips it this cycle (ADR-0070 #32). App task only.
    bool isBackedOff(const uint8_t* bssid) const;
    /// Index of @p bssid in the packed backoff table, or -1 if not tracked (ADR-0070 #32).
    int indexOfBackoff(const uint8_t* bssid) const;
    /// Insert @p bssid into the backoff table (miss count 0), evicting the least-penalised entry if it is
    /// full; returns its index (ADR-0070 #32). Eviction fails toward attempting — see kMaxBackoffEntries.
    int insertBackoff(const uint8_t* bssid);

    RadioSniffer& sniffer_;
    ApRegistry& registry_;
    CaptureReadyObserver& observer_;
    RawTransmitter* transmitter_;  // null = no raw-TX path; the default (invariant #16, ADR-0110 #5).
    bool deauthArmed_ = false;     // deauth transmits only with a transmitter AND this (ADR-0110 #5).
    bool solicitArmed_ = false;    // solicit transmits only with a transmitter AND this (ADR-0110 #5).
    const CrackedLookup* crackedLookup_ = nullptr;  // null = target every AP (gate B off; ADR-0061 #28).
    const SubmittedLookup* submittedLookup_ = nullptr;  // null = ignore submitted state (gate C off; ADR-0065 #30).
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
    bool belowFloorPass_ = false;   // false = attempting the >= kRssiFloor partition, true = the below one (ADR-0068 #31).
    uint32_t cycleCounter_ = 0;     // enumeration-cycle count (bumped at each discovery snapshot); drives backoff expiry (ADR-0070 #32).
    uint8_t parkedChannel_ = 0;     // the channel the radio last accepted.
    uint32_t deauthDueMs_ = 0;      // next-burst deadline while Capturing (armed); wrap-safe via reached().
    uint32_t deauthTxOk_ = 0;       // deauth/disassoc frames the seam accepted.
    uint32_t deauthTxFail_ = 0;     // deauth/disassoc frames the seam rejected.
    char capturingSsid_[33] = {0};  // the current target's SSID, copied at retarget for the solicit assoc.
    uint32_t solicitDueMs_ = 0;        // next solicit-burst (auth) deadline while Capturing; wrap-safe.
    uint32_t solicitAssocDueMs_ = 0;   // when the assoc of the in-flight burst may go out (auth time + gap).
    bool solicitAssocPending_ = false; // true between a sent auth and its assoc (the two-phase gap).
    uint32_t solicitTxOk_ = 0;         // solicit auth/assoc frames the seam accepted.
    uint32_t solicitTxFail_ = 0;       // solicit auth/assoc frames the seam rejected (incl. build failures).

    /// One tracked AP's capture-attempt history for the never-captured backoff (ADR-0070 #32). The table is
    /// a packed array scanned linearly (<= kMaxDiscoveredAps entries, app-task only), so no index structure
    /// earns its keep at this size (KISS).
    struct BackoffEntry {
        uint8_t bssid[6] = {0};
        uint16_t consecutiveMisses = 0;  // empty capture windows in a row; reset only by a capture.
        uint32_t backoffUntilCycle = 0;  // skipped while cycleCounter_ < this; 0 = not currently backed off.
    };
    BackoffEntry backoff_[kMaxBackoffEntries];
    size_t backoffCount_ = 0;  // occupied entries, packed in [0, backoffCount_).
};

}  // namespace sapper
