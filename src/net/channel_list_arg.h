/**
 * @file channel_list_arg.h
 * @brief Parse a comma-separated 2.4 GHz channel list ("1,6,11") from a build/config argument.
 *
 * Shared by the bench probes that take a hop list (rf_discover_probe, hunt_probe). It is pure and
 * header-only so both use one copy: two hand-rolled parsers of the same argument would be a place for
 * a silent divergence — one probe sweeping a band the other rejects — exactly the drift the quality
 * bar §3 forbids. Host-compilable, so the parse is unit-tested without a device.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "net/channel_hopper.h"

namespace sapper {

constexpr uint8_t kMinHopChannel = 1;   ///< 2.4 GHz channel floor.
constexpr uint8_t kMaxHopChannel = 14;  ///< 2.4 GHz channel ceiling.

/// Parse @p text ("1,6,11") into @p out, writing the count to @p count. Returns false on any
/// malformed or out-of-range token, an empty list, a trailing comma, a non-comma separator, or more
/// channels than a sweep holds — failing loud rather than sweeping a partial or wrong band (§3).
inline bool parseChannelListArg(const char* text, uint8_t out[kMaxHopChannels], size_t& count) {
    count = 0;
    if (text == nullptr) return false;
    const char* cursor = text;
    while (*cursor != '\0') {
        char* end = nullptr;
        const long channel = std::strtol(cursor, &end, 10);
        if (end == cursor) return false;  // not a number where one was expected.
        if (channel < kMinHopChannel || channel > kMaxHopChannel) return false;
        if (count >= kMaxHopChannels) return false;  // more channels than a sweep can hold.
        out[count++] = static_cast<uint8_t>(channel);
        cursor = end;
        if (*cursor == ',') {
            ++cursor;
            if (*cursor == '\0') return false;  // trailing comma: a malformed list, not a channel.
        } else if (*cursor != '\0') {
            return false;  // a separator other than a comma is malformed.
        }
    }
    return count > 0;
}

}  // namespace sapper
