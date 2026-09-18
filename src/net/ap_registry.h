/**
 * @file ap_registry.h
 * @brief Pure beacon-based AP discovery: record the distinct APs a sweep hears (ADR-0013).
 *
 * The FrameConsumer for a discovery run (as HandshakeConsumer is for a capture run, slice-0012). Its
 * onFrame() parses each beacon the sniffer delivers and records each distinct BSSID once — with its
 * SSID and operating channel — into a bounded table. All 802.11 interpretation stays in this pure
 * core (reusing eapol.h); the radio never reaches it, so it is host-unit-tested (ADR-0004 lane 1).
 *
 * Concurrency: on device, onFrame() runs in the Wi-Fi-driver task while the enumeration is read from
 * the app task (ADR-0013). The table is append-only and first-sighting-wins, so an entry is fully
 * written before count() publishes it and is never mutated afterward — a single-producer/
 * single-consumer discipline that lets the reader see whole entries without a lock. reset() and the
 * concurrent read-out are still the owner's to sequence against onFrame (the engine, slice-4 — the
 * same constraint the collector carries, LEDGER).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "net/radio_sniffer.h"

namespace sapper {

/// The most APs one sweep records before it reports overflow. Sized for a dense-but-realistic
/// environment; a fuller band sets overflowed() rather than silently dropping an AP (ADR-0013).
constexpr size_t kMaxDiscoveredAps = 64;

/// One discovered access point. `channel` is the beacon's DS-Parameter-Set channel, or the sweep
/// channel it was heard on when that element is absent (0 only if neither was known).
struct DiscoveredAp {
    uint8_t bssid[6] = {0};
    char ssid[33] = {0};  ///< Empty for a hidden network.
    uint8_t channel = 0;
};

/**
 * @brief Accumulates the distinct APs seen in a sweep from a stream of sniffed frames.
 *
 * onFrame() keeps a frame only if it is a beacon carrying a BSSID; a non-beacon or a headerless frame
 * is ignored. The first sighting of a BSSID is recorded and later sightings of it are dropped, so the
 * table holds each AP once. When the table is full a new BSSID sets overflowed() instead of being
 * silently discarded (quality bar §3).
 */
class ApRegistry : public FrameConsumer {
public:
    /// Record a beacon's AP (see class contract). Runs in the driver-task callback on device — a
    /// bounded, allocation-free parse, within §4 invariant #10 (ADR-0011 decision #3).
    void onFrame(const uint8_t* frame, uint16_t len) override;

    /// The channel the sweep is currently parked on, used as the fallback when a beacon carries no DS
    /// Parameter Set element. Set by the hop driver (app task) on each hop; read by onFrame.
    void setCurrentChannel(uint8_t channel) { currentChannel_.store(channel, std::memory_order_relaxed); }

    /// How many distinct APs have been recorded. Publishes the append-only table to a reader.
    size_t count() const { return count_.load(std::memory_order_acquire); }
    /// The @p i-th discovered AP; @p i must be < count().
    const DiscoveredAp& at(size_t i) const { return aps_[i]; }
    /// True once a new BSSID was dropped because the table was full — the enumeration is incomplete.
    bool overflowed() const { return overflowed_.load(std::memory_order_relaxed); }

    /// Discard every discovered AP. The owner sequences this against onFrame (LEDGER); not for use
    /// while a sweep is delivering frames.
    void reset();

private:
    /// Index of @p bssid in the recorded table, or -1 if not yet seen. Scans only published entries.
    int indexOf(const uint8_t* bssid) const;

    DiscoveredAp aps_[kMaxDiscoveredAps];
    std::atomic<size_t> count_{0};
    std::atomic<uint8_t> currentChannel_{0};
    std::atomic<bool> overflowed_{false};
};

}  // namespace sapper
