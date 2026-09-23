/**
 * @file rssi_histogram.cpp
 * @brief Pure RSSI↔capture accumulator implementation (ADR-0067).
 */
#include "net/rssi_histogram.h"

#include <cstring>

namespace sapper {
namespace {

// Band lower edges in dBm, strongest first: >= -50, -50..-60, -60..-70, -70..-80, < -80. Lower-inclusive,
// so a reading falls in the first band whose edge it meets or exceeds; below the last edge is the final
// (very-weak) band. kRssiBandCount-1 edges for kRssiBandCount bands.
constexpr int8_t kBandLowerEdge[kRssiBandCount - 1] = {-50, -60, -70, -80};

constexpr uint8_t kMagic[4] = {'R', 'S', 'S', 'H'};
constexpr uint8_t kVersion = 1;

/// Append a little-endian u32 to @p out at @p offset, advancing it.
void putU32(uint8_t* out, size_t& offset, uint32_t value) {
    out[offset++] = static_cast<uint8_t>(value & 0xFF);
    out[offset++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[offset++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[offset++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

/// Read a little-endian u32 from @p in at @p offset, advancing it.
uint32_t getU32(const uint8_t* in, size_t& offset) {
    const uint32_t value = static_cast<uint32_t>(in[offset]) |
                           (static_cast<uint32_t>(in[offset + 1]) << 8) |
                           (static_cast<uint32_t>(in[offset + 2]) << 16) |
                           (static_cast<uint32_t>(in[offset + 3]) << 24);
    offset += 4;
    return value;
}

}  // namespace

size_t rssiBand(int8_t rssi) {
    for (size_t i = 0; i < kRssiBandCount - 1; ++i) {
        if (rssi >= kBandLowerEdge[i]) return i;
    }
    return kRssiBandCount - 1;
}

void recordAttempt(RssiHistogram& hist, int8_t rssi, bool captured) {
    if (rssi == kRssiUnknown) {
        ++hist.naAttempts;
        if (captured) ++hist.naCaptured;
        return;
    }
    const size_t band = rssiBand(rssi);
    ++hist.attempts[band];
    if (captured) ++hist.captured[band];
}

void resetForTag(RssiHistogram& hist, const char* tag) {
    hist = RssiHistogram{};  // zero every counter.
    std::strncpy(hist.runTag, tag, kRssiRunTagLen - 1);
    hist.runTag[kRssiRunTagLen - 1] = '\0';
}

bool runTagFits(const char* tag) {
    return tag != nullptr && std::strlen(tag) < kRssiRunTagLen;
}

bool histogramHasTag(const RssiHistogram& hist, const char* tag) {
    if (tag == nullptr) return false;
    // Both are terminated within the field (the stored tag by resetForTag/deserialize, the given tag by
    // the runTagFits() gate), so a full-field bounded compare is exact — no truncation collision.
    return std::strncmp(hist.runTag, tag, kRssiRunTagLen) == 0;
}

size_t serializeHistogram(const RssiHistogram& hist, uint8_t* out, size_t capacity) {
    if (capacity < kRssiHistogramBytes) return 0;  // fail loud: never write a truncated record.
    size_t offset = 0;
    std::memcpy(out + offset, kMagic, sizeof(kMagic));
    offset += sizeof(kMagic);
    out[offset++] = kVersion;
    std::memcpy(out + offset, hist.runTag, kRssiRunTagLen);
    offset += kRssiRunTagLen;
    for (size_t i = 0; i < kRssiBandCount; ++i) putU32(out, offset, hist.attempts[i]);
    for (size_t i = 0; i < kRssiBandCount; ++i) putU32(out, offset, hist.captured[i]);
    putU32(out, offset, hist.naAttempts);
    putU32(out, offset, hist.naCaptured);
    return offset;
}

bool deserializeHistogram(const uint8_t* in, size_t len, RssiHistogram& hist) {
    if (len < kRssiHistogramBytes) return false;
    if (std::memcmp(in, kMagic, sizeof(kMagic)) != 0) return false;
    if (in[sizeof(kMagic)] != kVersion) return false;
    RssiHistogram parsed;
    size_t offset = sizeof(kMagic) + 1;  // past magic + version.
    std::memcpy(parsed.runTag, in + offset, kRssiRunTagLen);
    parsed.runTag[kRssiRunTagLen - 1] = '\0';  // defend against an unterminated stored tag.
    offset += kRssiRunTagLen;
    for (size_t i = 0; i < kRssiBandCount; ++i) parsed.attempts[i] = getU32(in, offset);
    for (size_t i = 0; i < kRssiBandCount; ++i) parsed.captured[i] = getU32(in, offset);
    parsed.naAttempts = getU32(in, offset);
    parsed.naCaptured = getU32(in, offset);
    hist = parsed;
    return true;
}

}  // namespace sapper
