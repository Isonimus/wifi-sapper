/**
 * @file maintenance_intent.h
 * @brief Pure fail-safe decode of the RTC "reboot into Maintenance" intent (ADR-0057).
 *
 * Maintenance on the Cardputer ADV is entered by pressing `m` while hunting: the loop persists a
 * reboot-intent and `ESP.restart()`s, and the next clean boot reads the intent and routes through the
 * existing pure `decideBootPhase()` gate — so Maintenance still enters only from a clean boot (§4 #21).
 *
 * The intent lives in an `RTC_NOINIT_ATTR` word: it survives the software restart we issue but is
 * *undefined* after a power-on/brownout. This header is the pure decision over that word — honour it
 * only on a genuine software reset carrying the sentinel, and consume it on read so a crash inside
 * Maintenance cannot loop the device back in. The RTC word itself and `esp_reset_reason()` are device
 * glue (main.cpp); the decision is host-tested here (the pure half of ADR-0057's split).
 */
#pragma once

#include <cstdint>

// esp_reset_reason_t. On device this is the real ESP-IDF header; on the native lane the identical
// angle-bracket include resolves to native/mock/esp_system.h via `-I native/mock` (ADR-0008), which
// defines just the enum. Only the symbol identity matters to the pure decision, never a numeric value.
#include <esp_system.h>

namespace sapper {

/// The sentinel written to the RTC intent word to request Maintenance on the next boot. "MAIN" in
/// ASCII — deliberately not 0: 0 is the cleared state and a plausible uninitialised/brownout value, so
/// a specific non-zero pattern makes RTC garbage that happens to survive a marginal reset far less
/// likely to be misread as a real intent (defence in depth behind the ESP_RST_SW gate below).
inline constexpr uint32_t kMaintenanceIntentMagic = 0x4D41494Eu;

/// The value the intent word holds when no Maintenance boot is pending. Writing it back on read is the
/// one-shot consume — any non-sentinel value reads as "no intent", so 0 is the natural cleared state.
inline constexpr uint32_t kMaintenanceIntentCleared = 0u;

/**
 * @brief Whether this boot should enter Maintenance, given the reset cause and the stored intent word.
 *
 * True only when BOTH hold: the reset was a software restart (`ESP_RST_SW` — the reset `ESP.restart()`
 * produces) AND the word equals `kMaintenanceIntentMagic`. A power-on or brownout (where the word is
 * undefined RTC garbage), or any non-sentinel word, is ignored — so a power-cycled or lost unit boots
 * to hunting, never silently into Maintenance. Fail-safe in the safe direction (ADR-0057 decision 3).
 */
bool shouldEnterMaintenanceOnBoot(esp_reset_reason_t resetReason, uint32_t storedWord);

/// The outcome of a one-shot intent read: whether to enter Maintenance, and the value the caller must
/// write back to the RTC word. `clearedWord` is always `kMaintenanceIntentCleared` — reading consumes
/// the intent unconditionally, so a re-evaluation on the *next* boot is false even if that boot is also
/// a software reset (a crash-in-Maintenance restart cannot re-enter Maintenance).
struct MaintenanceIntentRead {
    bool enterMaintenance;
    uint32_t clearedWord;
};

/**
 * @brief Read-and-consume the reboot intent: the `shouldEnterMaintenanceOnBoot` decision plus the
 *        cleared word to store back. Pure; the caller performs the actual RTC write (device glue).
 */
MaintenanceIntentRead consumeMaintenanceIntent(esp_reset_reason_t resetReason, uint32_t storedWord);

}  // namespace sapper
