/**
 * @file maintenance_intent.cpp
 * @brief Pure Maintenance reboot-intent decision (ADR-0057). Host-tested; no hardware.
 */
#include "config/maintenance_intent.h"

namespace sapper {

bool shouldEnterMaintenanceOnBoot(esp_reset_reason_t resetReason, uint32_t storedWord) {
    // Both gates, AND-ed: a software restart is the only reset under which the RTC word is defined (we
    // wrote it just before ESP.restart()), and the sentinel guards against a marginal reset that both
    // reports ESP_RST_SW and leaves plausible garbage. Either gate failing means "no intent" (safe).
    return resetReason == ESP_RST_SW && storedWord == kMaintenanceIntentMagic;
}

MaintenanceIntentRead consumeMaintenanceIntent(esp_reset_reason_t resetReason, uint32_t storedWord) {
    // The cleared word is unconditional: reading consumes the intent whether or not it fired, so a
    // reboot from *inside* Maintenance (also ESP_RST_SW) finds a cleared word and returns to hunting.
    return MaintenanceIntentRead{shouldEnterMaintenanceOnBoot(resetReason, storedWord),
                                 kMaintenanceIntentCleared};
}

}  // namespace sapper
