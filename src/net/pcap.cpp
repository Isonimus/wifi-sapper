/**
 * @file pcap.cpp
 * @brief Implementation of pcap serialization (ADR-0009).
 *
 * Header structs are written in host byte order. The classic pcap magic 0xa1b2c3d4 declares
 * little-endian to the reader, and both targets (ESP32-S3 and the x86 native lane) are
 * little-endian, so a native write is correct on the wire without byte-swapping.
 */
#include "net/pcap.h"

#include <cstring>
#include <initializer_list>

namespace sapper {
namespace {

struct __attribute__((packed)) PcapGlobalHeader {
    uint32_t magicNumber;
    uint16_t versionMajor;
    uint16_t versionMinor;
    int32_t thisZone;   ///< GMT-to-local correction; 0 (UTC).
    uint32_t sigFigs;   ///< Timestamp accuracy; 0, as every tool sets it.
    uint32_t snapLen;
    uint32_t network;   ///< Link type.
};

struct __attribute__((packed)) PcapRecordHeader {
    uint32_t tsSec;
    uint32_t tsUsec;
    uint32_t inclLen;   ///< Bytes stored: equals the full frame length (never truncated).
    uint32_t origLen;   ///< Original frame length: identical, since we store the whole frame.
};

bool writeGlobalHeader(CaptureSink& sink) {
    PcapGlobalHeader header{};
    header.magicNumber = kPcapMagicMicroseconds;
    header.versionMajor = kPcapVersionMajor;
    header.versionMinor = kPcapVersionMinor;
    header.thisZone = 0;
    header.sigFigs = 0;
    header.snapLen = kPcapSnapLen;
    header.network = kPcapLinkTypeIeee80211;
    return sink.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
}

// wpa-sec matches on frame content and the Beacon→M1→M2 ordering, not wall-clock time, so each
// record gets a synthetic increasing timestamp (its emission index) rather than a device clock the
// pure core does not have. Do not "fix" this by threading millis() through the pure layer — the
// crack does not depend on it (ADR-0009).
bool writeRecord(CaptureSink& sink, const CapturedFrame& frame, uint32_t index) {
    if (!frame.present()) return true;  // absent optional message (M3/M4): nothing to emit.
    PcapRecordHeader header{};
    header.tsSec = 0;
    header.tsUsec = index;
    header.inclLen = frame.len;
    header.origLen = frame.len;
    if (!sink.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header))) return false;
    return sink.write(frame.data, frame.len);
}

}  // namespace

bool serializeHandshake(const CapturedHandshake& handshake, CaptureSink& sink) {
    if (!handshake.isWpaSecValid()) return false;  // nothing uploadable to emit; write nothing.
    if (!writeGlobalHeader(sink)) return false;

    // Order matters: hcxpcapngtool pairs the SSID from the beacon with the nonces from M1/M2, so
    // the beacon leads, then the messages in sequence.
    uint32_t index = 0;
    if (!writeRecord(sink, handshake.beacon, index++)) return false;
    for (const HandshakeMessage message :
         {HandshakeMessage::M1, HandshakeMessage::M2, HandshakeMessage::M3, HandshakeMessage::M4}) {
        if (!handshake.has(message)) continue;
        const int slot = static_cast<int>(message) - static_cast<int>(HandshakeMessage::M1);
        if (!writeRecord(sink, handshake.msg[slot], index++)) return false;
    }
    return true;
}

}  // namespace sapper
