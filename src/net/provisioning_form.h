/**
 * @file provisioning_form.h
 * @brief Pure captive-portal form policy (ADR-0037): render the setup form from a secret-free model
 *        and merge a re-provision submission over the stored record.
 *
 * The hardware-free half of slice-0038 — no `WebServer`, `Preferences`, or `WiFi`. Host-tested
 * (ADR-0004 lane 1) so the two decisions that make the setup form *stateful without leaking a secret*
 * are proven off-device: what the form renders (`buildSetupForm`) and what a re-save persists
 * (`resolveProvisioningUpdate`). The captive portal (device-only) wires these to `loadProvisioning()`.
 */
#pragma once

#include <cstddef>

#include "net/provisioning_store.h"  // ProvisioningRecord

namespace sapper {

/**
 * @brief Everything the setup form needs to render, and *nothing else* (ADR-0037 §4 #20).
 *
 * Deliberately booleans only — no `ssid`, `pass`, `key`, or `webhookUrl` string. A provisioned secret
 * therefore cannot reach the served HTML, because `buildSetupForm()` is never handed one: the no-echo
 * guarantee holds by construction, mirroring the frame-free `CaptureFact` (#17) and the password-field-
 * free `WebhookNotification` (#19). The two `hasStored*` flags carry only the *fact* that a secret is
 * stored, so the field can invite "leave blank to keep" without exposing the value.
 */
struct SetupFormModel {
    bool deauthArmed;       ///< Render the deauth arm checkbox `checked`.
    bool notifyCaptured;    ///< Render the "captures" push checkbox `checked`.
    bool notifyCracked;     ///< Render the "cracked" push checkbox `checked`.
    bool notifySyncError;   ///< Render the "sync errors" push checkbox `checked`.
    bool hasStoredKey;      ///< A wpa-sec key is stored: drop `required`, show a "leave blank to keep" hint.
    bool hasStoredWebhook;  ///< A webhook URL is stored: show a "leave blank to keep" hint.
    bool hasStoredMaintenancePass;  ///< A Maintenance AP passphrase is stored (ADR-0039): show the same
                                    ///< "leave blank to keep" hint. Optional (empty = the AP falls back
                                    ///< to the published default), so never `required`. The value, being
                                    ///< a secret, is never rendered — only the fact that one is stored.
};

/// Buffer size for the rendered setup form. The form is ~1.4 KB; 2 KB leaves margin. A compile-time
/// size so `buildSetupForm` fails loud (returns 0) rather than truncating if it ever outgrows this.
constexpr size_t kSetupFormBufSize = 2048;

/**
 * @brief Render the setup form HTML from @p model into @p out. Pure; no secret can appear (see the
 *        model doc).
 * @return bytes written (excluding the NUL), or 0 if the form does not fit @p outSize — in which case
 *         @p out is left empty and the caller must serve an error, never a truncated page (§3).
 */
size_t buildSetupForm(const SetupFormModel& model, char* out, size_t outSize);

/**
 * @brief Decide the record to persist from a re-provision @p submitted over the @p stored record.
 *
 * - `ssid`, `pass`: always taken from the submission (the pair the operator re-enters; a blank
 *   passphrase stays an open network, ADR-0006 — so switching to open is reachable by retyping the SSID).
 * - `key`, `webhookUrl`, `maintenancePass`: a **blank** submitted field keeps the @p stored value (a
 *   re-save to fix WiFi must not silently wipe the secret the operator did not retype); a non-blank
 *   value replaces it. `maintenancePass` is a secret handled exactly like the others (ADR-0039 #6).
 * - the four notify/deauth booleans: always taken from the submission — the form now renders their
 *   stored state, so an unchecked box is a deliberate disarm, not a stateless reset.
 *
 * When @p hasStored is false (first boot) every field is taken from @p submitted verbatim, so a blank
 * key still fails `validateCredentials` loudly — "keep" only applies over an existing value.
 */
void resolveProvisioningUpdate(const ProvisioningRecord& submitted, const ProvisioningRecord& stored,
                               bool hasStored, ProvisioningRecord& out);

}  // namespace sapper
