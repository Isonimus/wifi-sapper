/**
 * @file fake_station_control.h
 * @brief StationControl double for the host lane (ADR-0017 decision #5; slice-0018).
 *
 * Lets a test decide whether the station "comes up" (associate + TLS-ready clock) and counts the
 * bring-up/tear-down calls, so the supervisor's arbitration sequencing and its backoff on a failed
 * bring-up are host-tested without a radio.
 */
#pragma once

#include "net/station_control.h"

namespace sapper_test {

class FakeStationControl : public sapper::StationControl {
public:
    bool bringUpStation() override {
        ++bringUpCalls;
        return bringUpSucceeds;
    }

    void tearDownStation() override { ++tearDownCalls; }

    // --- Test knobs and observation. ---
    bool bringUpSucceeds = true;  ///< false models offline / out-of-range / NTP failure.
    int bringUpCalls = 0;
    int tearDownCalls = 0;
};

}  // namespace sapper_test
