/**
 * @file handshake_collector.h
 * @brief Accumulate a WPA/WPA2 4-way handshake for one target BSSID (ADR-0009).
 *
 * The hardware-free "accumulate the set" half of the capture core. It is fed raw 802.11 frames
 * (as a promiscuous sniffer would later hand them over), keeps the ones that belong to its target
 * BSSID — the beacon and whichever of M1-M4 it sees — as **full frames**, and reports when it holds
 * enough to upload. It parses no nonces or MIC: `hcxpcapngtool` recovers those from the frames, so
 * extracting them on-device would be dead work (ADR-0009 decision #3). Host-unit-tested
 * (ADR-0004 lane 1); no radio, no filesystem, no `Serial`.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/eapol.h"

namespace sapper {

/// One stored 802.11 frame (beacon or an EAPOL message), kept whole for pcap export. A frame
/// longer than kMaxFrameLen is never stored (the collector rejects it), so `len <= kMaxFrameLen`.
struct CapturedFrame {
    uint8_t data[kMaxFrameLen];
    uint16_t len = 0;

    bool present() const { return len > 0; }
};

/// The frames and identity of one target's handshake. Populated by HandshakeCollector; consumed by
/// serializeHandshake() (net/pcap.h). `msg[0..3]` are M1..M4.
struct CapturedHandshake {
    uint8_t bssid[6] = {0};
    char ssid[33] = {0};    ///< From the beacon; empty for a hidden network.
    uint8_t channel = 0;    ///< The channel the collector was told to watch; for later filenames.
    CapturedFrame beacon;
    CapturedFrame msg[4];

    bool hasBeacon() const { return beacon.present(); }
    bool has(HandshakeMessage message) const;

    /// The minimum a wpa-sec upload needs: beacon + M1 + M2 (ADR-0009 decision #3).
    bool isWpaSecValid() const;
    /// All four messages captured. Reported, but not required for upload.
    bool isComplete() const;
};

/**
 * @brief Collects one target BSSID's handshake from a stream of sniffed frames.
 *
 * `ingest()` keeps a frame only if it belongs to the target BSSID and is either the beacon or a
 * recognised EAPOL message; everything else — a foreign BSSID, a non-EAPOL data frame, an
 * unidentifiable EAPOL frame, or a frame past kMaxFrameLen — is dropped. An over-long frame is
 * dropped rather than truncated: a truncated frame in a pcap is a silently corrupt capture, which
 * fail-loud forbids (quality bar §3).
 */
class HandshakeCollector {
public:
    /// @param targetBssid the 6-byte BSSID to collect; @param channel the watched channel (stored
    /// on the handshake for later filename use, 0 if unknown).
    explicit HandshakeCollector(const uint8_t targetBssid[6], uint8_t channel = 0);

    /// Feed one raw 802.11 frame from the sniffer.
    void ingest(const uint8_t* frame, uint16_t len);

    bool hasBeacon() const { return handshake_.hasBeacon(); }
    bool has(HandshakeMessage message) const { return handshake_.has(message); }
    bool isWpaSecValid() const { return handshake_.isWpaSecValid(); }
    bool isComplete() const { return handshake_.isComplete(); }
    const CapturedHandshake& handshake() const { return handshake_; }

    /// Discard everything collected, keeping the target BSSID and channel.
    void reset();

    /// Discard everything collected and aim at a new target BSSID and channel. The endless hunt
    /// re-uses one collector across its round-robin of targets, so it re-targets rather than
    /// value-copying a multi-KB collector per target (ADR-0015). reset() keeps the current target.
    void retarget(const uint8_t targetBssid[6], uint8_t channel);

private:
    bool matchesTarget(const uint8_t* frame, uint16_t len) const;

    CapturedHandshake handshake_;
};

}  // namespace sapper
