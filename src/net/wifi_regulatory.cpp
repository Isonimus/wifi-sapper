/**
 * @file wifi_regulatory.cpp
 * @brief esp_wifi implementation of the full-channel regulatory domain (ADR-0107). Device-only.
 */
#include "net/wifi_regulatory.h"

#ifndef UNIT_TEST

#include <Arduino.h>  // Serial (fail-loud log)
#include <esp_wifi.h>

namespace sapper {
namespace {

// JP deliberately (not US/EU): it is the only domain whose channel set spans the full 1-14, so a MANUAL
// policy over it never restricts a hop the hunt asks for. ch14 is 802.11b/Japan-only; it is inside the
// allowed domain but the shipped sweep does not visit it (kShippedHopChannels is 1-13).
constexpr char kRegulatoryCountry[3] = {'J', 'P', '\0'};
constexpr uint8_t kFirstChannel = 1;
constexpr uint8_t kChannelCount = 14;  // 1-14 inclusive.

}  // namespace

void applyFullChannelRegulatory() {
    wifi_country_t country = {};
    country.cc[0] = kRegulatoryCountry[0];
    country.cc[1] = kRegulatoryCountry[1];
    country.cc[2] = kRegulatoryCountry[2];
    country.schan = kFirstChannel;
    country.nchan = kChannelCount;
    country.policy = WIFI_COUNTRY_POLICY_MANUAL;
    // Non-fatal: if this fails the hunt still runs, but ch12-14 set_channel will be rejected (loud, per
    // channel, ADR-0015 #5). Log it rather than swallow it (§3) — it does not mask a broken invariant,
    // it reports a degraded band, so the headless appliance keeps hunting on the channels it can reach.
    const esp_err_t err = esp_wifi_set_country(&country);
    if (err != ESP_OK) {
        Serial.printf("[WiFi] set_country failed (%d); channels 12-14 may be restricted\n",
                      static_cast<int>(err));
    }
}

}  // namespace sapper

#endif  // UNIT_TEST
