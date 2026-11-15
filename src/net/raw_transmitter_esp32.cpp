/**
 * @file raw_transmitter_esp32.cpp
 * @brief esp_wifi_80211_tx implementation of the RawTransmitter seam (ADR-0027, ADR-0029). Device-only.
 */
#include "net/raw_transmitter_esp32.h"

#if !defined(UNIT_TEST)

#include <esp_wifi.h>

// The ESP-IDF SDK blocks raw management-frame TX (deauth 0xC0 / disassoc 0xA0). The pioarduino
// platform (Bruce's patched libs) exposes ieee80211_raw_frame_sanity_check as a weak symbol; this
// strong override returns 1 for the magic argument to allow the frame, matching the Marauder/Bruce
// convention (ADR-0027). This symbol IS the raw-TX capability; it now ships in every device build
// (ADR-0029 lifted ADR-0027 decision 3's SAPPER_TEST_HOOKS gate), guarded at *runtime* by the
// operator's default-off deauth arm toggle rather than at compile time (§4 invariant #16). The
// weaken_deauth_pre.py pre-script already ran for the shipped env, so this override links cleanly.
namespace {
constexpr int32_t kSanityBypassMagic = 31337;  // Marauder/Bruce magic value; not ours to choose.
}  // namespace

extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) {
    (void)arg2;
    (void)arg3;
    if (arg == kSanityBypassMagic) return 1;  // allow the raw frame.
    return 0;                                  // block everything else (keeps some SDK safety).
}

namespace sapper {

bool Esp32RawTransmitter::transmit(const uint8_t* frame, uint16_t len) {
    // esp_wifi_80211_tx transmits a raw frame on an active interface; the sniffer has already brought
    // the STA interface up promiscuous on the target channel. en_sys_seq=false: we own the sequence
    // field (the builder zeroes it), so let the driver leave it untouched.
    return esp_wifi_80211_tx(WIFI_IF_STA, frame, len, false) == ESP_OK;
}

}  // namespace sapper

#endif  // !UNIT_TEST
