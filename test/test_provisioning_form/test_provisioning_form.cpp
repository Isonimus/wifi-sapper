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

void test_form_never_prefills_a_value(void) {
    // The no-secret-echo guarantee, observably: the form uses placeholders, never a `value=` attribute,
    // so no field — secret or not — is pre-filled. The model has no secret string to echo (§4 #20), and
    // this pins the property so a future edit that adds `value='...'` fails here.
    SetupFormModel model = {};
    model.hasStoredKey = true;
    model.hasStoredWebhook = true;
    char html[kSetupFormBufSize];
    TEST_ASSERT_NOT_EQUAL(0, buildSetupForm(model, html, sizeof(html)));
    TEST_ASSERT_NULL(strstr(html, "value="));
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
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, out);

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
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, out);

    TEST_ASSERT_EQUAL_STRING("newkey", out.key);
    TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/new", out.webhookUrl);
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
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, out);

    TEST_ASSERT_FALSE(out.deauthEnabled);
    TEST_ASSERT_FALSE(out.notifyCracked);
    TEST_ASSERT_TRUE(out.notifyCaptured);
    TEST_ASSERT_TRUE(out.notifySyncError);
}

void test_blank_pass_stays_open_not_kept(void) {
    // The passphrase is part of the pair the operator re-enters, so a blank one is an open network —
    // NOT the stored pass kept. This is what keeps "switch to open" reachable through the portal.
    ProvisioningRecord stored;
    setRecord(stored, "old", "oldpass12", "k", "");
    ProvisioningRecord submitted;
    setRecord(submitted, "old", "", "k", "");  // blank passphrase = open
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/true, out);

    TEST_ASSERT_EQUAL_STRING("", out.pass);  // open, not the stored "oldpass12"
}

void test_first_boot_takes_submission_verbatim_and_blank_key_fails(void) {
    // With nothing stored, "keep" applies to nothing: a blank key stays blank and fails validation
    // loudly, exactly as before this slice.
    ProvisioningRecord stored;
    std::memset(&stored, 0, sizeof(stored));
    ProvisioningRecord submitted;
    setRecord(submitted, "net", "password1", "", "");  // blank key on first boot
    ProvisioningRecord out;
    resolveProvisioningUpdate(submitted, stored, /*hasStored=*/false, out);

    TEST_ASSERT_EQUAL_STRING("", out.key);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(CredentialError::KeyEmpty),
                          static_cast<int>(validateCredentials(out.ssid, out.pass, out.key)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_checkboxes_reflect_model);
    RUN_TEST(test_stored_key_drops_required_and_hints_keep);
    RUN_TEST(test_absent_key_keeps_required);
    RUN_TEST(test_stored_webhook_hints_keep);
    RUN_TEST(test_absent_webhook_shows_default_placeholder);
    RUN_TEST(test_form_never_prefills_a_value);
    RUN_TEST(test_overflow_returns_zero_and_empties);
    RUN_TEST(test_blank_secrets_keep_stored);
    RUN_TEST(test_nonblank_secrets_replace);
    RUN_TEST(test_toggles_and_pair_always_from_submission);
    RUN_TEST(test_blank_pass_stays_open_not_kept);
    RUN_TEST(test_first_boot_takes_submission_verbatim_and_blank_key_fails);
    return UNITY_END();
}
