/**
 * @file radio_sniffer.h
 * @brief The radio->raw-bytes RX seam: promiscuous 802.11 capture on one fixed channel (ADR-0011).
 *
 * The hardware-facing half of the capture path. A RadioSniffer owns promiscuous mode on a single
 * channel and hands every received 802.11 frame to a FrameConsumer as raw bytes — no parsing, no
 * filtering in code (ADR-0009 decision #1; ADR-0011). The pure capture core (eapol.h,
 * handshake_collector.h) is a consumer; it never sees the radio. This header is pure: the abstract
 * seam compiles on the native lane so a host test drives a consumer through the same onFrame() the
 * device callback uses (CLAUDE.md §4 invariant #2). The ESP-IDF implementation is device-only
 * (radio_sniffer_esp32.h).
 */
#pragma once

#include <cstdint>

namespace sapper {

/// Receives raw 802.11 frames from a RadioSniffer.
///
/// On-device, onFrame() is invoked in the Wi-Fi driver task context (ADR-0011): it must copy what
/// it needs and return promptly — no blocking, no heap allocation, no heavy protocol work in the
/// callback. Feeding the frame into the pure core (a bounded memcpy) is within that budget; anything
/// heavier belongs on the engine side, after the frame has left this context.
class FrameConsumer {
public:
    virtual ~FrameConsumer() = default;
    /// Handle one raw 802.11 frame of @p len bytes (including any trailing FCS the radio delivered).
    virtual void onFrame(const uint8_t* frame, uint16_t len) = 0;
};

/// Owns promiscuous mode on one fixed channel and forwards received frames to a FrameConsumer.
/// Channel hopping and AP discovery are deferred to later slices (ADR-0011); this seam sits on the
/// channel it is told to watch.
class RadioSniffer {
public:
    virtual ~RadioSniffer() = default;
    /// Enter promiscuous mode on @p channel (1-based) and deliver frames to @p consumer. Returns
    /// false if the radio could not be placed in promiscuous mode. The consumer must outlive the
    /// sniffer's started state.
    virtual bool begin(uint8_t channel, FrameConsumer& consumer) = 0;
    /// Leave promiscuous mode and detach the consumer. A device implementation cannot hard-join a
    /// callback already in flight (blocking in the driver callback is forbidden — ADR-0011, §4
    /// invariant #10), so a frame dequeued just before stop() may still complete its onFrame() after
    /// stop() returns; a caller that reads the consumer's state must sequence that itself (the engine
    /// owns this — LEDGER). Safe to call when not started.
    virtual void stop() = 0;
};

}  // namespace sapper
