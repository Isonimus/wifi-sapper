/**
 * @file ap_registry.cpp
 * @brief Implementation of the pure beacon-based AP registry (ADR-0013).
 */
#include "net/ap_registry.h"

#include <cstring>

#include "net/eapol.h"

namespace sapper {

int ApRegistry::indexOf(const uint8_t* bssid) const {
    // Only the producer (onFrame) calls this, so a relaxed load of its own count is enough; the
    // release-store below is what publishes new entries to the app-task reader.
    const size_t n = count_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) {
        if (std::memcmp(aps_[i].bssid, bssid, sizeof(aps_[i].bssid)) == 0) return static_cast<int>(i);
    }
    return -1;
}

void ApRegistry::onFrame(const uint8_t* frame, uint16_t len) {
    if (!isBeacon(frame, len)) return;  // discovery reads beacons; ignore everything else.
    const uint8_t* bssid = frameBssid(frame, len);
    if (bssid == nullptr) return;  // too short to carry a BSSID.

    if (indexOf(bssid) >= 0) return;  // first sighting wins; the AP is already recorded.

    const size_t n = count_.load(std::memory_order_relaxed);
    if (n >= kMaxDiscoveredAps) {
        overflowed_.store(true, std::memory_order_relaxed);  // full: report, never silently drop.
        return;
    }

    // Fill the entry completely before publishing it: the release-store on count_ is what makes these
    // writes visible to the app-task reader, which reads count_ with acquire and only then the entry.
    DiscoveredAp& slot = aps_[n];
    std::memcpy(slot.bssid, bssid, sizeof(slot.bssid));
    beaconSsid(frame, len, slot.ssid);  // empty on a hidden or malformed-SSID network; still recorded.
    const uint8_t advertised = beaconChannel(frame, len);
    // The beacon's own DS-Parameter-Set channel is authoritative; fall back to the channel we are
    // sweeping only when the beacon does not state one (ADR-0013 decision #3).
    slot.channel = (advertised != 0) ? advertised : currentChannel_.load(std::memory_order_relaxed);

    count_.store(n + 1, std::memory_order_release);
}

void ApRegistry::reset() {
    count_.store(0, std::memory_order_release);
    overflowed_.store(false, std::memory_order_relaxed);
}

}  // namespace sapper
