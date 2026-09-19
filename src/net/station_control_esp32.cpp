/**
 * @file station_control_esp32.cpp
 * @brief Device backing of the StationControl seam (ADR-0017 decision #5). Device-only.
 */
#include "net/station_control_esp32.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <WiFi.h>

#include "net/wifi_station.h"

namespace sapper {

bool Esp32StationControl::bringUpStation() {
    // The engine's stop() already left promiscuous mode; ensure a clean STA role, then associate.
    WiFi.mode(WIFI_STA);
    if (!connectStation(ssid_, pass_, nullptr)) {
        Serial.println("[UPLOAD] station associate failed — deferring drain");
        return false;
    }
    // A pinned certificate cannot be validated against a 1970 clock (ADR-0017 decision #1), so the
    // station is "up" for upload only once NTP has set a real time. syncClock() is idempotent — it
    // returns at once when the clock is already real.
    if (!syncClock(nullptr)) {
        Serial.println("[UPLOAD] NTP sync failed — deferring drain (TLS needs a real clock)");
        WiFi.disconnect(/*wifioff=*/false);
        return false;
    }
    return true;
}

void Esp32StationControl::tearDownStation() {
    // Drop the association but keep the radio on; the engine's begin() re-enters promiscuous mode
    // (which itself sets WIFI_STA + disconnect) when the supervisor resumes the hunt.
    WiFi.disconnect(/*wifioff=*/false);
}

}  // namespace sapper

#endif  // UNIT_TEST
