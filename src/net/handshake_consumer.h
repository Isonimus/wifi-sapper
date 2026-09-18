/**
 * @file handshake_consumer.h
 * @brief Bridge the RX seam to the pure capture core: feed sniffed frames to a HandshakeCollector.
 *
 * The one adapter that connects a RadioSniffer (radio_sniffer.h) to the pure accumulator
 * (handshake_collector.h). It is the concrete FrameConsumer the device wires under the promiscuous
 * callback and the host test drives directly (CLAUDE.md §4 invariant #2 — stimulus enters through
 * the same seam a real frame uses). Pure and header-only: it holds a collector by reference and
 * forwards each frame; all judgement lives in the collector it wraps (ADR-0011).
 */
#pragma once

#include <cstdint>

#include "net/handshake_collector.h"
#include "net/radio_sniffer.h"

namespace sapper {

/// Forwards every sniffed frame into a HandshakeCollector. onFrame() is a bounded copy — ingest()
/// memcpy's a matching frame and returns — which is all the Wi-Fi-driver callback context allows
/// (ADR-0011).
class HandshakeConsumer : public FrameConsumer {
public:
    explicit HandshakeConsumer(HandshakeCollector& collector) : collector_(collector) {}
    void onFrame(const uint8_t* frame, uint16_t len) override { collector_.ingest(frame, len); }

private:
    HandshakeCollector& collector_;
};

}  // namespace sapper
