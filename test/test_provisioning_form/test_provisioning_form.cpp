/**
 * @file test_provisioning_form.cpp
 * @brief Native unit tests for the pure captive-portal form policy (ADR-0037, slice-0038).
 *
 * Proves the setup form is stateful without leaking a secret: `buildSetupForm` renders the stored
 * toggle state and "leave blank to keep" hints but never a secret value (§4 #20 holds by construction —
 * the model carries no secret string), and `resolveProvisioningUpdate` keeps a stored key/webhook when
 * its field is submitted blank while taking the SSID/passphrase pair and the toggles from the submission.
 * The device portal that wires these to loadProvisioning()/persistProvisioning() is proven on hardware
 * by the slice-0007 provisioning verify.
 */
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "net/provisioning.h"       // validateCredentials, CredentialError
#include "net/provisioning_form.h"  // SetupFormModel, buildSetupForm, resolveProvisioningUpdate
#include "net/portal_theme.h"       // slice-0098: the shared theme/wordmark the form carries

using namespace sapper;

void setUp(void) {}
void tearDown(void) {}

namespace {

// Populate a record's strings; toggles set separately per test. snprintf (not strcpy) so the bounded
// copy is warning-clean under -Wall -Wextra.
void setRecord(ProvisioningRecord& r, const char* ssid, const char* pass, const char* key,
               const char* webhook) {
    std::memset(&r, 0, sizeof(r));
    std::snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
    std::snprintf(r.pass, sizeof(r.pass), "%s", pass);
    std::snprintf(r.key, sizeof(r.key), "%s", key);
    std::snprintf(r.webhookUrl, sizeof(r.webhookUrl), "%s", webhook);
}

}  // namespace

// --- slice-0098: the form carries the shared banner theme + wordmark in the head -------------------

void test_form_carries_the_theme_and_wordmark_in_the_head(void) {
    // Worst-case model (every "leave blank to keep" hint + maxed dim fields) must still render and fit the
    // buffer with the theme + wordmark added. The exact field markup is pinned by the no-echo tests below;
    // this pins that the brand assets are present and sit in <head>, before <body>.
    SetupFormModel model = {};
    model.hasStoredNetwork = model.hasStoredKey = model.hasStoredWebhook =
        model.hasStoredMaintenancePass = true;
    model.screenDimAfterSec = model.screenOffAfterSec = 65535;
    char html[kSetupFormBufSize];
    TEST_ASSERT_GREATER_THAN(0u, buildSetupForm(model, html, sizeof(html)));
    const char* css = std::strstr(html, kPortalThemeCss);
    const char* svg = std::strstr(html, kPortalWordmarkSvg);
    TEST_ASSERT_NOT_NULL(css);
    TEST_ASSERT_NOT_NULL(svg);
    // The CSS sits in <head> (before <body>); the wordmark is the visible page header (just after <body>).
    const char* body = std::strstr(html, "<body");
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_TRUE(css < body);
    TEST_ASSERT_TRUE(svg > body);
}

// --- buildSetupForm: the checkbox state renders back -------------------------

void test_checkboxes_reflect_model(void) {
    SetupFormModel model = {};
    model.deauthArmed = true;
    model.notifyCaptured = true;
    model.notifyCracked = false;
    model.notifySyncError = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));

    // A set flag emits ` checked` right before the closing `>`; an unset one closes immediately.
    TEST_ASSERT_NOT_NULL(strstr(html, "name='deauth' type='checkbox' checked>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='nCaptured' type='checkbox' checked>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='nSyncErr' type='checkbox' checked>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='nCracked' type='checkbox'>"));  // NOT checked
    TEST_ASSERT_NULL(strstr(html, "name='nCracked' type='checkbox' checked>"));
}

// --- buildSetupForm: "a secret is stored" invites keep, never echoes it ------

void test_stored_key_drops_required_and_hints_keep(void) {
    SetupFormModel model = {};
    model.hasStoredKey = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current key'"));
    // With a key stored the field must NOT be `required`, so a re-save can keep it with a blank field.
    TEST_ASSERT_NULL(strstr(html, "name='key' maxlength='64' required"));
}

void test_absent_key_keeps_required(void) {
    SetupFormModel model = {};  // hasStoredKey false
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='key' maxlength='64' required"));
}

void test_stored_webhook_hints_keep(void) {
    SetupFormModel model = {};
    model.hasStoredWebhook = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current webhook'"));
}

void test_absent_webhook_shows_default_placeholder(void) {
    SetupFormModel model = {};  // hasStoredWebhook false
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='https://ntfy.sh/your-topic'"));
}

void test_stored_maintpass_hints_keep(void) {
    // The Maintenance AP passphrase (ADR-0039 #6) is a secret handled like the key/webhook: a stored one
    // invites "leave blank to keep", and the value itself is never rendered (only the fact one is stored).
    SetupFormModel model = {};
    model.hasStoredMaintenancePass = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current passphrase'"));
}

void test_absent_maintpass_shows_default_placeholder(void) {
    SetupFormModel model = {};  // hasStoredMaintenancePass false
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='blank = use default AP password'"));
}

// --- buildSetupForm: Wi-Fi keep-on-blank-SSID + the open-network checkbox (ADR-0106) ----------

void test_stored_network_drops_ssid_required_and_hints_keep(void) {
    // With a network stored, the SSID is no longer `required` (so a blank submit keeps it) and both Wi-Fi
    // fields invite "leave blank to keep" — never echoing the stored SSID value (§4 #20 holds here too).
    SetupFormModel model = {};
    model.hasStoredNetwork = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current Wi-Fi'>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current Wi-Fi password'>"));
    // The SSID input must NOT be `required` when a network is stored.
    TEST_ASSERT_NULL(strstr(html, "spellcheck='false' required>"));
}

void test_absent_network_keeps_ssid_required(void) {
    // First boot (no network stored): the SSID stays `required` so an empty first provisioning is refused.
    SetupFormModel model = {};  // hasStoredNetwork false
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "spellcheck='false' required>"));
    TEST_ASSERT_NULL(strstr(html, "placeholder='leave blank to keep current Wi-Fi'>"));
    // The passphrase teaches the WPA2 minimum rather than "keep", and the Open box handles no-passphrase.
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='at least 8 characters'>"));
}

void test_open_network_checkbox_is_rendered(void) {
    // The explicit open-network checkbox is always present (ADR-0106): open is ticked, never inferred.
    SetupFormModel model = {};
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='open' type='checkbox'>"));
}

void test_form_never_prefills_a_secret_value(void) {
    // The no-secret-echo guarantee, observably: every SECRET field renders via a placeholder and closes
    // without a `value=` attribute, so no stored secret is pre-filled. The model carries no secret string
    // to echo (§4 #20); this pins that no `value='...'` is ever attached to a secret input. (The
    // non-secret screen-dim numbers DO carry value=, by design, and are checked separately below.) Every
    // secret-bearing flag is set so the assertion covers the maintenance passphrase field too (ADR-0039 #6).
    SetupFormModel model = {};
    model.hasStoredKey = true;
    model.hasStoredWebhook = true;
    model.hasStoredMaintenancePass = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    // Each secret input closes at '>' right after its placeholder/last attribute: no value= was injected.
    // The passphrase now carries a placeholder too (ADR-0106) but still no value (no stored value to echo).
    TEST_ASSERT_NOT_NULL(strstr(html, "name='pass' type='password' maxlength='63' placeholder='at least 8 characters'>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current key'>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current webhook'>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "placeholder='leave blank to keep current passphrase'>"));
    // The only value= attributes in the form are the two numeric dim fields (type='number').
    TEST_ASSERT_NULL(strstr(html, "type='password' maxlength='63' value="));
    TEST_ASSERT_NULL(strstr(html, "type='url' maxlength='160' placeholder='leave blank to keep current webhook' value="));
}

void test_ssid_field_suppresses_mobile_autocapitalize(void) {
    // A phone keyboard autocapitalising/autocorrecting the SSID stores a wrong (case-sensitive) name, a
    // silent credential error that fails association. The plain-text SSID field must carry the suppression
    // attributes (the password/url fields are shielded by their input type). Pin it so a future edit
    // cannot drop them.
    SetupFormModel model = {};
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='ssid'"));
    TEST_ASSERT_NOT_NULL(strstr(html, "autocapitalize='none'"));
    TEST_ASSERT_NOT_NULL(strstr(html, "autocorrect='off'"));
    TEST_ASSERT_NOT_NULL(strstr(html, "spellcheck='false'"));
}

void test_form_prefills_the_screen_dim_delays_from_the_model(void) {
    // The two inactivity-dim number fields render the stored value back (value='N'), so a re-save to change
    // something else preserves them instead of wiping to 0 — the fix for the re-save-clobbers-config defect.
    SetupFormModel model = {};
    model.screenDimAfterSec = 20;
    model.screenOffAfterSec = 60;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='dimAfter' type='number' min='0' max='65535' inputmode='numeric' value='20'>"));
    TEST_ASSERT_NOT_NULL(strstr(html, "name='offAfter' type='number' min='0' max='65535' inputmode='numeric' value='60'>"));
}

void test_parse_delay_seconds_clamps_out_of_range(void) {
    // Server-side bound (ADR-0093): the form max is advisory, so a raw POST value is clamped, never wrapped.
    TEST_ASSERT_EQUAL_UINT16(60, parseDelaySeconds("60"));
    TEST_ASSERT_EQUAL_UINT16(65535, parseDelaySeconds("65535"));
    TEST_ASSERT_EQUAL_UINT16(65535, parseDelaySeconds("70000"));  // over-range -> capped, not 70000-65536.
    TEST_ASSERT_EQUAL_UINT16(0, parseDelaySeconds("-1"));         // negative -> 0, not 65535 via wrap.
    TEST_ASSERT_EQUAL_UINT16(0, parseDelaySeconds(""));           // blank -> off.
    TEST_ASSERT_EQUAL_UINT16(0, parseDelaySeconds("abc"));        // non-numeric -> off.
}

void test_resolve_clamps_off_delay_at_or_above_dim_delay(void) {
    // A nonzero off delay below the dim delay is clamped up so the readable dim stage is never skipped;
    // applies on first boot (no stored) too, and a 0 off (dim-only) is left as-is (ADR-0093 decision 5).
    ProvisioningRecord submitted;
    setRecord(submitted, "net", "passABCDEFGH", "keyABCDEFGH", "");
    submitted.screenDimAfterSec = 30;
    submitted.screenOffAfterSec = 10;  // below dim: must clamp up to 30.
    ProvisioningRecord stored = {};
    ProvisioningRecord out = {};
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/false, /*openNetwork=*/false, out);
    TEST_ASSERT_EQUAL_UINT16(30, out.screenDimAfterSec);
    TEST_ASSERT_EQUAL_UINT16(30, out.screenOffAfterSec);

    submitted.screenOffAfterSec = 0;  // dim-only: left as-is.
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/false, /*openNetwork=*/false, out);
    TEST_ASSERT_EQUAL_UINT16(0, out.screenOffAfterSec);

    submitted.screenOffAfterSec = 90;  // above dim: kept.
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/false, /*openNetwork=*/false, out);
    TEST_ASSERT_EQUAL_UINT16(90, out.screenOffAfterSec);
}

void test_overflow_returns_zero_and_empties(void) {
    // Too small a buffer must fail loud (return 0, write nothing), never serve a truncated half-form.
    SetupFormModel model = {};
    char tiny[16];
    tiny[0] = 'x';  // sentinel: must be cleared on failure.
    TEST_ASSERT_EQUAL_UINT(0, buildSetupForm(model, tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_UINT8('\0', tiny[0]);
}

// --- resolveProvisioningUpdate: the re-save merge ---------------------------

void test_blank_secrets_keep_stored(void) {
    // The core defect fix: re-save to fix WiFi, leaving key + webhook blank, keeps them (ADR-0037).
    ProvisioningRecord stored;
    setRecord(stored, "old-ssid", "oldpass12", "storedkey", "https://ntfy.sh/kept");
    ProvisioningRecord submitted;
    setRecord(submitted, "new-ssid", "newpass12", "", "");  // key + webhook left blank
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("storedkey", out.key);              // kept
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/kept", out.webhookUrl);  // kept
    TEST_ASSERT_EQUAL_STRING("new-ssid", out.ssid);             // from submission
    TEST_ASSERT_EQUAL_STRING("newpass12", out.pass);            // from submission
}

void test_nonblank_secrets_replace(void) {
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "storedkey", "https://ntfy.sh/old");
    ProvisioningRecord submitted;
    setRecord(submitted, "old", "oldpass12", "newkey", "https://ntfy.sh/new");
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("newkey", out.key);
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/new", out.webhookUrl);
}

void test_maintpass_blank_keeps_stored_nonblank_replaces(void) {
    // The Maintenance passphrase (ADR-0039 #6) merges like the key/webhook: a blank re-save keeps the
    // stored value (a WiFi fix must not silently drop the AP back to the default password), a non-blank
    // value replaces it.
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "k", "");
    std::snprintf(stored.maintenancePass, sizeof(stored.maintenancePass), "storedmaint1");

    ProvisioningRecord submittedBlank;
    setRecord(submittedBlank, "new", "newpass12", "", "");  // maintpass blank
    ProvisioningRecord keptOut;
    resolveProvisioningUpdate(submittedBlank, stored, /*hasStored=*/true, /*openNetwork=*/false, keptOut);
    TEST_ASSERT_EQUAL_STRING("storedmaint1", keptOut.maintenancePass);  // kept

    ProvisioningRecord submittedNew;
    setRecord(submittedNew, "new", "newpass12", "", "");
    std::snprintf(submittedNew.maintenancePass, sizeof(submittedNew.maintenancePass), "freshmaint99");
    ProvisioningRecord replacedOut;
    resolveProvisioningUpdate(submittedNew, stored, /*hasStored=*/true, /*openNetwork=*/false, replacedOut);
    TEST_ASSERT_EQUAL_STRING("freshmaint99", replacedOut.maintenancePass);  // replaced
}

void test_toggles_and_pair_always_from_submission(void) {
    // The form now renders stored toggle state, so an unchecked box is a deliberate disarm: the merge
    // must always take the submitted booleans, never keep the stored ones.
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "k", "https://ntfy.sh/x");
    stored.deauthEnabled = true;
    stored.notifyCracked = true;
    stored.notifyCaptured = true;
    ProvisioningRecord submitted;
    setRecord(submitted, "old", "oldpass12", "k", "https://ntfy.sh/x");
    submitted.deauthEnabled = false;    // deliberate disarm
    submitted.notifyCracked = false;    // deliberate off
    submitted.notifyCaptured = true;
    submitted.notifySyncError = true;   // deliberate on
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_FALSE(out.deauthEnabled);
    TEST_ASSERT_FALSE(out.notifyCracked);
    TEST_ASSERT_TRUE(out.notifyCaptured);
    TEST_ASSERT_TRUE(out.notifySyncError);
}

void test_blank_ssid_keeps_the_whole_network_pair(void) {
    // ADR-0106 core: a blank SSID on a provisioned device keeps BOTH the stored ssid and pass, so the
    // light-touch edit (change a toggle/secret) needs no network re-entry and the pair is never split.
    ProvisioningRecord stored;
    setRecord(stored, "home-net", "homepass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "", "", "k", "");  // SSID + pass both blank = keep current Wi-Fi
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("home-net", out.ssid);   // kept
    TEST_ASSERT_EQUAL_STRING("homepass12", out.pass);  // kept together with the SSID, not wiped to open
}

void test_blank_ssid_with_a_typed_pass_rotates_the_passphrase(void) {
    // Rotating the router passphrase on the SAME network: keep the name (SSID blank), type the new
    // passphrase (ADR-0106). The kept SSID is unambiguous, so the new passphrase is bound to it — the old
    // passphrase must NOT be kept, or the unit reboots with a stale password and strands on association.
    ProvisioningRecord stored;
    setRecord(stored, "home-net", "oldpass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "", "rotated-pass99", "k", "");  // SSID blank (keep), new passphrase typed
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("home-net", out.ssid);        // network name kept
    TEST_ASSERT_EQUAL_STRING("rotated-pass99", out.pass);  // the new passphrase, NOT the stale stored one
}

void test_blank_ssid_with_open_makes_the_current_network_open(void) {
    // Switching the current network to open without re-typing its name: SSID blank + Open ticked keeps the
    // name and clears the passphrase (ADR-0106, closing the ADR-0037 same-SSID-to-open limitation).
    ProvisioningRecord stored;
    setRecord(stored, "home-net", "oldpass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "", "", "k", "");  // SSID + pass blank, but Open ticked below
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/true, out);

    TEST_ASSERT_EQUAL_STRING("home-net", out.ssid);  // name kept
    TEST_ASSERT_EQUAL_STRING("", out.pass);          // now open
}

void test_open_checkbox_forces_empty_pass_for_a_new_network(void) {
    // Ticking Open while entering a new SSID stores an open network unambiguously: the checkbox wins over
    // any stray passphrase text (ADR-0106), so the record is open even if the field held something.
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "cafe-wifi", "ignored-text", "k", "");
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/true, out);

    TEST_ASSERT_EQUAL_STRING("cafe-wifi", out.ssid);  // the new network
    TEST_ASSERT_EQUAL_STRING("", out.pass);           // forced open by the checkbox
}

void test_new_ssid_takes_the_submitted_pair(void) {
    // A non-blank SSID is a network switch: both the SSID and passphrase come from the submission.
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "new-net", "newpass34", "k", "");
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("new-net", out.ssid);
    TEST_ASSERT_EQUAL_STRING("newpass34", out.pass);
}

void test_validate_network_entry_guards_the_silent_open_footgun(void) {
    // The brick guard (ADR-0106): a network being SET with a blank passphrase and Open unticked is
    // refused; keeping the current network, an explicit Open, or a supplied passphrase all pass.
    // Keep current network (stored, blank SSID): fine regardless of pass/open.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry("", "", false, /*hasStored=*/true)));
    // New network with a passphrase: fine (length is validateCredentials' job).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry("new", "pass1234", false, /*hasStored=*/true)));
    // New network explicitly open: fine.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry("new", "", true, /*hasStored=*/true)));
    // New network, blank pass, Open unticked: the footgun — refused.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::PassphraseRequired),
                          static_cast<int>(validateNetworkEntry("new", "", false, /*hasStored=*/true)));
    // First boot (nothing stored), SSID entered, blank pass, Open unticked: also refused (no silent open).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::PassphraseRequired),
                          static_cast<int>(validateNetworkEntry("new", "", false, /*hasStored=*/false)));
    // First boot with a passphrase: fine.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry("new", "pass1234", false, /*hasStored=*/false)));
    // Defensive nullptr handling (the device path never passes null, but the guard must not deref): a null
    // SSID on a provisioned device reads as "keep current" (None); a null pass on a new network reads as
    // blank, so Open unticked -> refused, Open ticked -> fine.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry(nullptr, nullptr, false, /*hasStored=*/true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::PassphraseRequired),
                          static_cast<int>(validateNetworkEntry("new", nullptr, false, /*hasStored=*/true)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(NetworkEntryError::None),
                          static_cast<int>(validateNetworkEntry("new", nullptr, true, /*hasStored=*/true)));
}

void test_first_boot_takes_submission_verbatim_and_blank_key_fails(void) {
    // With nothing stored, "keep" applies to nothing: a blank key stays blank and fails validation
    // loudly, exactly as before this slice.
    ProvisioningRecord stored;
    std::memset(&stored, 0, sizeof(stored));
    ProvisioningRecord submitted;
    setRecord(submitted, "net", "password1", "", "");  // blank key on first boot
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/false, /*openNetwork=*/false, out);

    TEST_ASSERT_EQUAL_STRING("", out.key);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::KeyEmpty),
                          static_cast<int>(validateCredentials(out.ssid, out.pass, out.key)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_form_carries_the_theme_and_wordmark_in_the_head);
    RUN_TEST(test_checkboxes_reflect_model);
    RUN_TEST(test_stored_key_drops_required_and_hints_keep);
    RUN_TEST(test_absent_key_keeps_required);
    RUN_TEST(test_stored_webhook_hints_keep);
    RUN_TEST(test_absent_webhook_shows_default_placeholder);
    RUN_TEST(test_stored_maintpass_hints_keep);
    RUN_TEST(test_absent_maintpass_shows_default_placeholder);
    RUN_TEST(test_stored_network_drops_ssid_required_and_hints_keep);
    RUN_TEST(test_absent_network_keeps_ssid_required);
    RUN_TEST(test_open_network_checkbox_is_rendered);
    RUN_TEST(test_ssid_field_suppresses_mobile_autocapitalize);
    RUN_TEST(test_form_prefills_the_screen_dim_delays_from_the_model);
    RUN_TEST(test_parse_delay_seconds_clamps_out_of_range);
    RUN_TEST(test_resolve_clamps_off_delay_at_or_above_dim_delay);
    RUN_TEST(test_form_never_prefills_a_secret_value);
    RUN_TEST(test_overflow_returns_zero_and_empties);
    RUN_TEST(test_blank_secrets_keep_stored);
    RUN_TEST(test_nonblank_secrets_replace);
    RUN_TEST(test_maintpass_blank_keeps_stored_nonblank_replaces);
    RUN_TEST(test_toggles_and_pair_always_from_submission);
    RUN_TEST(test_blank_ssid_keeps_the_whole_network_pair);
    RUN_TEST(test_blank_ssid_with_a_typed_pass_rotates_the_passphrase);
    RUN_TEST(test_blank_ssid_with_open_makes_the_current_network_open);
    RUN_TEST(test_open_checkbox_forces_empty_pass_for_a_new_network);
    RUN_TEST(test_new_ssid_takes_the_submitted_pair);
    RUN_TEST(test_validate_network_entry_guards_the_silent_open_footgun);
    RUN_TEST(test_first_boot_takes_submission_verbatim_and_blank_key_fails);
    return UNITY_END();
}
