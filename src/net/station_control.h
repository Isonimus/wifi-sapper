/**
 * @file station_control.h
 * @brief The radio-arbitration seam: put the radio into an upload-ready station role and return it
 *        to the hunt (ADR-0017 decision #5).
 *
 * Uploading needs the radio associated as a station with a TLS-valid clock; hunting needs it
 * promiscuous. They are mutually exclusive (ADR-0013 decision #6), so the drain supervisor pauses the
 * hunt, brings the station up through this seam, drains, and tears it back down. Wrapping the
 * device's WiFi mode change + connectStation()/syncClock() (wifi_station.h) behind this one interface
 * keeps the supervisor's *sequencing* pure and host-tested against a fake — the supervisor never
 * touches WiFi directly, and the device wiring is one thin adapter (station_control_esp32.h).
 *
 * bringUpStation() folds association and the NTP clock sync into a single "TLS-ready" contract: a
 * pinned certificate cannot be validated against a 1970 clock (ADR-0017 decision #1; wifi_station.h),
 * so returning true only when both the association and a real clock are in hand means the supervisor
 * has one honest gate to check before it uploads, rather than sequencing two calls itself.
 */
#pragma once

namespace sapper {

/// Switches the radio between promiscuous hunting and an upload-ready station role. Host-faked for
/// the supervisor tests; device-backed by the real WiFi/NTP glue.
class StationControl {
public:
    virtual ~StationControl() = default;

    /// Leave promiscuous mode, associate as a station, and ensure a real (NTP-synced) clock. Returns
    /// true only when the station is up **and** the clock is TLS-valid — i.e. an upload may proceed.
    /// Returns false (having torn down any partial state) if association or the clock sync failed, so
    /// the supervisor backs off rather than attempting a TLS upload that cannot validate the pin.
    virtual bool bringUpStation() = 0;

    /// Return the radio from the station role so promiscuous hunting can resume. Always safe to call.
    virtual void tearDownStation() = 0;
};

}  // namespace sapper
