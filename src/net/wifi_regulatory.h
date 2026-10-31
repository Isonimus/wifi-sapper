/**
 * @file wifi_regulatory.h
 * @brief Pin the 2.4 GHz regulatory domain to the full channel set (ADR-0107). Device-only.
 *
 * Carried from the sibling Adversary repo's `wifi_utils::applyFullChannelRegulatory`. The ESP32 Wi-Fi
 * stack resets the regulatory country to the restrictive 1-11 default on every bring-up (`WiFi.mode()` /
 * stack re-init), so `esp_wifi_set_channel(12..14)` is then rejected and ch12-13 APs (standard under
 * ETSI, i.e. most of Europe) are silently missed. This must therefore be re-applied after *every* WiFi
 * bring-up that tunes to or associates on an arbitrary channel, never once (§4 #44).
 */
#pragma once

namespace sapper {

/**
 * @brief Set the regulatory domain to the full 1-14 2.4 GHz channel set (country JP, MANUAL policy).
 *
 * Call immediately after each `WiFi.mode(<non-OFF>)` on the promiscuous hunt sniffer, the upload/sync STA
 * window, and the boot STA connect (the uplink itself may be on ch12-13). No-op on the native lane (the
 * body is `esp_wifi`, excluded from the host build). The appliance transmits on ch12-14 regardless of
 * location as a result (a deliberate trade, README-documented; lawful use is the operator's, like §4 #16).
 */
void applyFullChannelRegulatory();

}  // namespace sapper
