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
    bool pmkidSolicit;      ///< Render the PMKID-solicit arm checkbox `checked` (ADR-0110 #5d). A toggle,
                            ///< not a secret, so its stored state renders back like the deauth arm.
    bool notifyCaptured;    ///< Render the "captures" push checkbox `checked`.
    bool notifyCracked;     ///< Render the "cracked" push checkbox `checked`.
    bool notifySyncError;   ///< Render the "sync errors" push checkbox `checked`.
    bool hasStoredNetwork;  ///< A Wi-Fi network is stored (ADR-0106): drop the SSID `required`, show a
                            ///< "leave blank to keep current Wi-Fi" hint on the SSID and passphrase. The
                            ///< SSID value is never rendered (only the fact one is stored), so §4 #20
                            ///< holds by construction here too — unlike the secrets, the SSID is not a
                            ///< secret, but echoing it on the public setup AP would still leak the home
                            ///< network name, so it is treated the same no-echo way.
    bool hasStoredKey;      ///< A wpa-sec key is stored: drop `required`, show a "leave blank to keep" hint.
    bool hasStoredWebhook;  ///< A webhook URL is stored: show a "leave blank to keep" hint.
    bool hasStoredMaintenancePass;  ///< A Maintenance AP passphrase is stored (ADR-0039): show the same
                                    ///< "leave blank to keep" hint. Optional (empty = the AP falls back
                                    ///< to the published default), so never `required`. The value, being
                                    ///< a secret, is never rendered — only the fact that one is stored.
    bool hasStoredPwncrackKey;  ///< A pwncrack API key is stored (ADR-0109 #46): show the "leave blank to
                                ///< keep" hint. Optional (empty = the pwncrack second backend stays
                                ///< unwired, so a wpa-sec-only device is unchanged), so never `required`.
                                ///< The value, being a secret, is never rendered — only the fact one is
                                ///< stored, handled no-echo like the wpa-sec key/webhook/maintenance pass.
    /// Inactivity screen-dim delays in seconds (ADR-0093), prefilled into the two number inputs so a
    /// re-save preserves them like the checkboxes preserve their state (these are config, not secrets, so
    /// rendering the value is safe — unlike the secret fields above, which only ever carry a stored-flag).
    uint16_t screenDimAfterSec;
    uint16_t screenOffAfterSec;
};

/// Buffer size for the rendered setup form. Since slice-0098 the form carries the shared portal theme
/// (CSS 1620 B + the SVG wordmark 621 B, injected into the head); ADR-0106 added the Wi-Fi keep/open
/// markup, ADR-0109 the optional pwncrack-key field, and ADR-0110 the PMKID-solicit arm checkbox + help
/// line (~241 B). The true worst case (every "leave blank to keep" placeholder, the Open checkbox + help
/// line, both arm checkboxes `checked` at +8 B each, and both dim fields at 65535) is ~4999 B; 5632 B
/// leaves ~633 B margin. The pinning test test_form_carries_the_theme_and_wordmark_in_the_head renders a
/// zeroed model (every box unchecked), so it asserts only that this fit holds, ~40 B under the checked
/// worst case. A compile-time size so `buildSetupForm` fails loud (returns 0) rather than truncating if it
/// ever outgrows this, which is the real guard regardless of the exact worst-case byte count.
constexpr size_t kSetupFormBufSize = 5632;

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
 * - `ssid`, `pass`: keep-on-blank-SSID (ADR-0106). On a provisioned device a **blank** submitted SSID
 *   keeps the stored network *name*; the passphrase is then set against that kept name — @p openNetwork
 *   makes it open, a blank passphrase keeps the stored one (the full light-touch edit), and a non-blank
 *   passphrase rotates it (change the password without re-typing the SSID). A non-blank submitted SSID is
 *   a network switch: the passphrase comes wholly from the submission (so a stored passphrase is never
 *   bound to a different SSID), with @p openNetwork forcing it empty. In every "setting a network" case
 *   (new SSID or first boot) a blank passphrase with Open unticked means "open" is *not* chosen here; that
 *   case is refused upstream by `validateNetworkEntry`, never silently stored open — the brick footgun.
 * - `key`, `webhookUrl`, `maintenancePass`, `pwncrackKey`: a **blank** submitted field keeps the @p stored
 *   value (a re-save to fix WiFi must not silently wipe the secret the operator did not retype); a non-blank
 *   value replaces it. `maintenancePass` is a secret handled exactly like the others (ADR-0039 #6), and
 *   `pwncrackKey` likewise (ADR-0109 #46): a blank re-save keeps the pwncrack backend wired, a non-blank
 *   value rotates the key, and clearing it to unwire pwncrack is a deliberate separate action (not a blank).
 * - the five notify/deauth/pmkid-solicit booleans: always taken from the submission — the form now
 *   renders their stored state, so an unchecked box is a deliberate disarm, not a stateless reset.
 *
 * When @p hasStored is false (first boot) the SSID/pass are taken from @p submitted (with @p openNetwork
 * clearing the passphrase) and the secrets verbatim, so a blank key still fails `validateCredentials`
 * loudly — "keep" only applies over an existing value.
 */
void resolveProvisioningUpdate(const ProvisioningRecord& submitted, const ProvisioningRecord& stored,
                               bool hasStored, bool openNetwork, ProvisioningRecord& out);

/**
 * @brief Why a submitted Wi-Fi network entry was rejected by the form policy. `None` means usable.
 *
 * Distinct from `CredentialError` (which validates the *merged* record's field lengths): this reasons
 * about the operator's *intent* from the submission + the Open checkbox, which the merged record alone
 * cannot reconstruct (an empty passphrase looks identical whether it was meant as open or forgotten).
 */
enum class NetworkEntryError : uint8_t {
    None,
    PassphraseRequired,  ///< A new network was entered with no passphrase and Open was not ticked.
};

/**
 * @brief Guard the silent-open brick footgun (ADR-0106). Pure.
 *
 * Returns `PassphraseRequired` when a network is being *set* (a non-blank @p submittedSsid, or first boot
 * when @p hasStored is false) with a **blank** @p submittedPass and @p openNetwork **false** — the case
 * where "blank" used to be silently stored as an open network, bricking association. Keeping the current
 * network (a stored record with @p submittedSsid blank) and an explicit @p openNetwork are both `None`;
 * a supplied passphrase is `None` here (its length is `validateCredentials`' job). Run alongside
 * `validateCredentials` at each save site, which still catches an empty SSID / key and the length bounds.
 */
NetworkEntryError validateNetworkEntry(const char* submittedSsid, const char* submittedPass,
                                       bool openNetwork, bool hasStored);

/**
 * @brief Parse a screen-dim delay field (seconds) from form text, clamped to a u16 (ADR-0093).
 *
 * The form's `max='65535'` is a client-side hint only; this handler is reachable by a raw POST, so the
 * value is bounded server-side: a negative or non-numeric value is 0 (off), an over-range value is capped
 * at 65535, never wrapped. Pure so the bound is host-tested (the string fields already reject over-length
 * loudly; a dim delay is non-critical config, so it is clamped to a sane value rather than 400-rejected).
 */
uint16_t parseDelaySeconds(const char* text);

/**
 * @brief A read-only view over a submitted form's fields, so the submission parser is host-testable.
 *
 * The one seam that lets `parseSetupFormSubmission` be pure (ADR-0114): the device wraps a live
 * `WebServer` (`net/web_arg_reader.h`), the native lane a fake map. `value(name)` returns the
 * submitted value, or the empty string (never `nullptr`) for an absent field, mirroring
 * `WebServer::arg`'s contract. The returned pointer must stay valid only until the next `value()`
 * call on the same reader (the device adapter reuses one scratch buffer); the parser honours this by
 * copying or consuming each value before it reads the next, so no two values are ever held at once.
 */
struct FormArgReader {
    virtual const char* value(const char* name) const = 0;

protected:
    ~FormArgReader() = default;  // not deleted through this type; no virtual dtor needed, no vtable cost.
};

/// Why `parseSetupFormSubmission` rejected a submission. `None` means the record was populated.
enum class FormParseError : uint8_t {
    None,
    FieldTooLong,  ///< A string field exceeded its record buffer — rejected, never truncated (see below).
};

/**
 * @brief Parse a re-provision submission from @p args into @p submitted and @p openNetwork. Pure.
 *
 * The single owner of the setup form's field-name list and per-field parsing — the one place both
 * serving surfaces (the captive portal and the Maintenance `/config` editor) read a POST into a
 * `ProvisioningRecord`, so the list cannot drift between them (ADR-0114, revising ADR-0043 decision 6).
 * It zeroes @p submitted first, then: copies each string field rejecting an over-length value rather
 * than truncating it (a truncated SSID could slip past `validateCredentials` as a shorter network),
 * reads every checkbox (`"on"` = set, absent = clear — a rendered-stored-state form makes an unchecked
 * box a deliberate disarm), and clamps each screen-dim delay to a u16 (`parseDelaySeconds`). It does
 * **not** merge, validate, or persist — the caller still runs `resolveProvisioningUpdate` over the
 * stored record, `validateCredentials`/`validateNetworkEntry`, and `persistProvisioning`.
 *
 * @return `FieldTooLong` (with @p submitted left partially written and unusable) if any string field
 *         did not fit; otherwise `None` with @p submitted fully populated and @p openNetwork set.
 */
FormParseError parseSetupFormSubmission(const FormArgReader& args, ProvisioningRecord& submitted,
                                        bool& openNetwork);

}  // namespace sapper
