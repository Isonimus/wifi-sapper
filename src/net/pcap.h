/**
 * @file pcap.h
 * @brief Serialize a captured handshake to a wpa-sec-uploadable pcap (ADR-0009).
 *
 * The hardware-free "emit the bytes" half of the capture core. It writes a classic pcap —
 * link type 105 (`LINKTYPE_IEEE802_11`), raw 802.11 frames, no radiotap (ADR-0009 decision #2) —
 * into an abstract `CaptureSink`. The sink is the seam ADR-0009 leaves open: the device backs it
 * with SD / LittleFS / an in-RAM upload buffer (slice-5), and a host test backs it with a plain
 * byte buffer. This layer never opens a file (CLAUDE.md §4 invariant #9).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/handshake_collector.h"

namespace sapper {

/// Where serialized pcap bytes go. The one seam capture output flows through (invariant #9); the
/// storage medium is a later decision (ADR-0009). A write must report failure so the serializer can
/// fail loud rather than emit a truncated, silently corrupt pcap (quality bar §3).
class CaptureSink {
public:
    virtual ~CaptureSink() = default;
    /// Append @p len bytes. Returns false on any short or failed write.
    virtual bool write(const uint8_t* data, size_t len) = 0;
};

/**
 * @brief Serialize @p handshake's frames as a pcap into @p sink.
 *
 * Writes the global header then the Beacon, M1, and M2 records (and M3/M4 when present) in that
 * order, each as a 16-byte record header followed by the full 802.11 frame. Returns false without
 * writing anything if @p handshake is not `isWpaSecValid()` (no uploadable capture to emit), and
 * false if @p sink fails any write partway through. Returns true only when a complete, valid pcap
 * reached the sink.
 */
bool serializeHandshake(const CapturedHandshake& handshake, CaptureSink& sink);

// pcap constants, exposed so a test asserts against named values rather than magic numbers.
constexpr uint32_t kPcapMagicMicroseconds = 0xa1b2c3d4;  ///< Little-endian, microsecond timestamps.
constexpr uint16_t kPcapVersionMajor = 2;
constexpr uint16_t kPcapVersionMinor = 4;
constexpr uint32_t kPcapSnapLen = 65535;
constexpr uint32_t kPcapLinkTypeIeee80211 = 105;  ///< LINKTYPE_IEEE802_11: raw frames, no radiotap.

// Sizes of the two headers serializeHandshake writes, named so a caller can bound a buffer against
// the format rather than a magic number. The global header prefixes the file once; a record header
// prefixes each stored frame.
constexpr size_t kPcapGlobalHeaderLen = 24;
constexpr size_t kPcapRecordHeaderLen = 16;
/// The most frames one wpa-sec upload holds: the beacon plus M1-M4 (CapturedHandshake).
constexpr size_t kMaxHandshakeFrames = 5;
/// Upper bound on a serialized handshake pcap: the global header plus every frame at its maximum
/// length behind a record header. Lets the in-RAM upload buffer and the CaptureStore size a fixed
/// buffer that can never be overrun by serializeHandshake (quality bar §3 — no silent truncation).
constexpr size_t kMaxSerializedPcapLen =
    kPcapGlobalHeaderLen + kMaxHandshakeFrames * (kPcapRecordHeaderLen + kMaxFrameLen);

}  // namespace sapper
