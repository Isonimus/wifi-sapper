/**
 * @file channel_hopper.h
 * @brief Pure schedule for sweeping the radio across a channel list (ADR-0013).
 *
 * The "which channel, and when to move" half of channel hopping, with no radio in it. Given a channel
 * list, a per-channel dwell time, and a clock the caller supplies, it decides the current channel and
 * when to advance to the next (wrapping at the end). The caller applies the current channel to the
 * radio through the RadioSniffer::setChannel seam; the hopper never touches hardware, so it is
 * host-unit-tested against a fake clock (ADR-0004 lane 1). No Serial, no millis() of its own.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// The most channels a single sweep can hold: the full 2.4 GHz set (1-14) with headroom is 14.
constexpr size_t kMaxHopChannels = 14;

/**
 * @brief Cycles a channel index on a fixed dwell, driven by a caller-supplied clock.
 *
 * Construction copies the channel list and parks on the first channel. `tick(now)` returns true when
 * the dwell has elapsed since the last hop — having advanced to the next channel — and false
 * otherwise; the caller retunes the radio to `currentChannel()` whenever `tick` returns true. The
 * clock is passed in, so the whole sweep is deterministic under test.
 */
class ChannelHopper {
public:
    /// @param channels the 1-based channel list to sweep (copied; 1..kMaxHopChannels entries);
    /// @param count how many entries (clamped to kMaxHopChannels; must be >= 1); @param dwellMs how
    /// long to stay on each channel before hopping.
    ChannelHopper(const uint8_t* channels, size_t count, uint32_t dwellMs);

    /// The channel to be tuned to right now.
    uint8_t currentChannel() const;

    /// The number of channels in the sweep.
    size_t channelCount() const { return count_; }

    /// Advance to the next channel (wrapping) if @p nowMs is at least one dwell past the last hop.
    /// Returns true when it hopped (the caller then retunes to currentChannel()), false if it is not
    /// yet time. A single-channel sweep still "hops" onto itself so the caller's retune stays honest.
    bool tick(uint32_t nowMs);

    /// Re-park on the first channel and start the dwell clock at @p nowMs.
    void reset(uint32_t nowMs);

private:
    uint8_t channels_[kMaxHopChannels] = {0};
    size_t count_ = 0;
    size_t index_ = 0;
    uint32_t dwellMs_ = 0;
    uint32_t lastHopMs_ = 0;
};

}  // namespace sapper
