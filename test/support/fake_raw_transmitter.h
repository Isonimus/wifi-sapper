/**
 * @file fake_raw_transmitter.h
 * @brief A test RawTransmitter that records the frames it is handed (ADR-0027).
 *
 * Lets a host test prove the builder->seam contract (§4 invariant #2): the exact bytes the pure
 * builder produced reach the transmitter unchanged, with no radio. `frames` is the sequence of
 * transmitted frames in order.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "net/raw_transmitter.h"

namespace sapper_test {

class FakeRawTransmitter : public sapper::RawTransmitter {
public:
    std::vector<std::vector<uint8_t>> frames;
    bool nextResult = true;  ///< What transmit() returns; flip to simulate a radio rejection.

    bool transmit(const uint8_t* frame, uint16_t len) override {
        frames.emplace_back(frame, frame + len);
        return nextResult;
    }

    size_t count() const { return frames.size(); }
    const std::vector<uint8_t>& last() const { return frames.back(); }
};

}  // namespace sapper_test
