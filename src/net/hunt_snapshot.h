/**
 * @file hunt_snapshot.h
 * @brief The read-only pull snapshot of live hunt state, and the seam a surface reads it through (ADR-0033).
 *
 * §4 invariant #13 lets a surface read engine state "only … [via] the pull snapshot the on-air verifies
 * use", and keeps the EventBus for discrete facts. Live in-flight capture state — which network is being
 * sniffed and which of Beacon/M1–M4 have arrived — changes many times per capture, so it must NOT ride
 * the synchronous bus (it would make every onAppEvent a hot path). This is that pull: a POD the engine
 * fills on demand and a surface folds into its view (§4 invariant #18).
 *
 * The read is deliberately best-effort/cosmetic: the collector is written on the driver task (onFrame,
 * §4 #10) while a surface reads on the app task, with no lock. The message flags are single bytes and
 * monotonic within a capture, and the target is stable for a Capturing phase, so a torn read is at worst
 * a one-tick-stale indicator or a briefly garbled SSID glyph — self-correcting next tick (ADR-0033 #3).
 *
 * Pure and hardware-free (a POD + one abstract method), so the whole HUD policy is host-tested against a
 * fake source.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// The hunt loop's phase. Owned here (not nested in HuntEngine) so this POD header need not include the
/// engine; HuntEngine aliases it as `HuntEngine::Phase`, so existing `HuntEngine::Phase::X` still names it.
enum class HuntPhase : uint8_t { Idle, Discovering, Capturing, Quiescing };

/// A coarse signal-strength bucket for the capturing target (ADR-0116 decision 5). The HUD draws it as a
/// multi-bar glyph; only the bucket reaches ScreenView (compared in operator==), so raw-dBm dither never
/// forces a redraw — the same discipline the battery gauge uses for charge (ADR-0091, §4 #40). Unknown is
/// "no beacon RSSI recorded for this target" (the registry's kRssiUnset sentinel), which hides the glyph.
enum class RssiLevel : uint8_t { Unknown, Weak, Fair, Good, Strong };

/// Bucket a real discovery-time RSSI (dBm) into the coarse HUD level. Candidate thresholds (ADR-0002 §5),
/// tunable on the panel: -55/-67/-78 dBm split four bars. Never returns Unknown — the engine assigns that
/// for the kRssiUnset sentinel before calling, so this pure header stays free of the ap_registry include.
inline RssiLevel rssiBucket(int8_t dBm) {
    if (dBm >= -55) return RssiLevel::Strong;
    if (dBm >= -67) return RssiLevel::Good;
    if (dBm >= -78) return RssiLevel::Fair;
    return RssiLevel::Weak;
}

/// A snapshot of what the appliance is doing right now. Identity + progress only — no pcap frame bytes
/// (those stay quarantined to the CaptureSink seam, §4 #9); a surface renders it and nothing more.
struct HuntSnapshot {
    HuntPhase phase = HuntPhase::Idle;
    uint8_t channel = 0;        ///< The parked channel (meaningful once Discovering/Capturing).
    size_t discovered = 0;      ///< APs discovered so far this sweep.
    uint8_t bssid[6] = {0};     ///< The target being captured (zeroed when not Capturing).
    char ssid[33] = {0};        ///< The target's SSID; empty for a hidden network.
    bool hasBeacon = false;
    bool hasM1 = false;
    bool hasM2 = false;
    bool hasM3 = false;
    bool hasM4 = false;
    RssiLevel rssi = RssiLevel::Unknown;  ///< Coarse signal of the capturing target (ADR-0116 #5); bucketed.
    bool hasPmkid = false;                ///< The held M1 carries a PMKID KDE — the clientless terminal (ADR-0110).

    /// How many of the five handshake pieces (Beacon + M1..M4) are present — the progress-bar numerator.
    uint8_t collectedCount() const {
        return static_cast<uint8_t>(hasBeacon + hasM1 + hasM2 + hasM3 + hasM4);
    }
};

/// The seam a surface pulls the snapshot through. HuntEngine implements it; a fake implements it for the
/// host tests. const: reading the snapshot mutates no engine state.
class HuntSnapshotSource {
public:
    virtual ~HuntSnapshotSource() = default;
    virtual HuntSnapshot huntSnapshot() const = 0;
};

}  // namespace sapper
