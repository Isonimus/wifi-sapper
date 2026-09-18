/**
 * @file channel_hopper.cpp
 * @brief Implementation of the pure channel-hop schedule (ADR-0013).
 */
#include "net/channel_hopper.h"

#include <algorithm>
#include <cstring>

namespace sapper {

ChannelHopper::ChannelHopper(const uint8_t* channels, size_t count, uint32_t dwellMs)
    : dwellMs_(dwellMs) {
    // Clamp rather than overflow the fixed buffer: a caller asking for more channels than the 2.4 GHz
    // band holds is a bug the probe guards, but the copy must stay in bounds regardless.
    count_ = (channels == nullptr) ? 0 : std::min(count, kMaxHopChannels);
    if (count_ > 0) std::memcpy(channels_, channels, count_);
}

uint8_t ChannelHopper::currentChannel() const {
    if (count_ == 0) return 0;  // empty sweep: no channel to be on (probe rejects this upstream).
    return channels_[index_];
}

bool ChannelHopper::tick(uint32_t nowMs) {
    if (count_ == 0) return false;
    // Unsigned subtraction is correct across the 49.7-day millis() wrap: (now - last) is the elapsed
    // interval even when the counter wrapped once between the two reads.
    if (nowMs - lastHopMs_ < dwellMs_) return false;
    index_ = (index_ + 1) % count_;
    lastHopMs_ = nowMs;
    return true;
}

void ChannelHopper::reset(uint32_t nowMs) {
    index_ = 0;
    lastHopMs_ = nowMs;
}

}  // namespace sapper
