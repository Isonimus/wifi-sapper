/**
 * @file fake_radio_sniffer.h
 * @brief A host test double for the RadioSniffer seam (ADR-0011), driving the HuntEngine without a
 *        radio.
 *
 * It records the channel it was begun/retuned to, lets a test reject configured channels (a
 * country-restricted channel, as the real setChannel can — ADR-0013), and delivers test frames into
 * whichever consumer the engine installed — the same onFrame() the device callback would call (§4
 * invariant #2). Frames delivered while stopped or before begin() go nowhere, mirroring the real seam.
 */
#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "net/radio_sniffer.h"

namespace sapper_test {

class FakeRadioSniffer : public sapper::RadioSniffer {
public:
    bool begin(uint8_t channel, sapper::FrameConsumer& consumer) override {
        if (!beginSucceeds) return false;
        consumer_ = &consumer;
        started_ = true;
        currentChannel = channel;
        channelHistory.push_back(channel);
        return true;
    }

    bool setChannel(uint8_t channel) override {
        setChannelCalls.push_back(channel);
        if (rejectedChannels.count(channel) != 0) return false;  // e.g. country-restricted.
        currentChannel = channel;
        channelHistory.push_back(channel);
        return true;
    }

    void stop() override { started_ = false; }

    /// Deliver one raw frame into the installed consumer, as the promiscuous callback would.
    void deliver(const std::vector<uint8_t>& frame) {
        if (started_ && consumer_ != nullptr) {
            consumer_->onFrame(frame.data(), static_cast<uint16_t>(frame.size()));
        }
    }

    // --- Test knobs and observation. ---
    bool beginSucceeds = true;
    std::set<uint8_t> rejectedChannels;   ///< setChannel returns false for these.
    uint8_t currentChannel = 0;           ///< The channel the radio is actually on.
    std::vector<uint8_t> channelHistory;  ///< Every channel begin()/setChannel() landed on.
    std::vector<uint8_t> setChannelCalls; ///< Every channel setChannel() was asked for (incl. rejects).

private:
    sapper::FrameConsumer* consumer_ = nullptr;
    bool started_ = false;
};

}  // namespace sapper_test
