/**
 * @file station_control_esp32.h
 * @brief Device backing of the StationControl seam over the existing WiFi/NTP glue (ADR-0017
 *        decision #5). Device-only.
 *
 * The one thin adapter between the pure drain supervisor and the radio: it drives the promiscuous↔STA
 * mode change and the existing connectStation()/syncClock() (wifi_station.h), so all the supervisor's
 * sequencing stays host-tested against a fake and only this wiring is device-only. It is constructed
 * with the provisioned credentials (read once through the NVS seam — §4 invariant #6) and holds only
 * pointers to them, so the backing strings must outlive the adapter; hunt_loop owns a copy for the
 * device's whole run and constructs the adapter from it (hunt_loop.cpp).
 */
#pragma once

#ifndef UNIT_TEST

#include "net/station_control.h"

namespace sapper {

class Esp32StationControl : public StationControl {
public:
    /// @param ssid,pass the provisioned network credentials; must outlive this adapter.
    Esp32StationControl(const char* ssid, const char* pass) : ssid_(ssid), pass_(pass) {}

    bool bringUpStation() override;
    void tearDownStation() override;

private:
    const char* ssid_;
    const char* pass_;
};

}  // namespace sapper

#endif  // UNIT_TEST
