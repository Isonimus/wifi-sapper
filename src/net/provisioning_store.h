/**
 * @file provisioning_store.h
 * @brief The NVS seam for provisioned secrets (ADR-0006, CLAUDE.md §4 invariant #6).
 *
 * Device-only: wraps Arduino `Preferences` (NVS), so it is excluded from the native lane and
 * proven on hardware by the slice-0007 verify script (Scenarios C, D). The pure validation and
 * boot-gate logic these functions rest on live in provisioning.h and are host-tested (lane 1).
 *
 * Single writer, one reader path (invariant #6): `persistProvisioning()` is the *only* function
 * that writes credentials, `loadProvisioning()` the only one that reads them, `clearProvisioning()`
 * the only one that erases them. No call site touches the `sapper` NVS namespace directly, so a
 * grep for the namespace name finds exactly this file — that is what makes the invariant checkable.
 *
 * Provisioning is deliberately NOT reachable from the serial vocabulary (invariant #7): nothing
 * here is wired to a serial command. Under `SAPPER_TEST_HOOKS` the bench harness seeds a record
 * through `persistProvisioning()` at boot via compile-time build flags — a build-time path, never
 * a runtime serial actuation.
 */
#pragma once

#include "net/provisioning.h"

namespace sapper {

/**
 * @brief A credential triad as stored in NVS. NUL-terminated C strings, one over each protocol
 *        maximum so a value at the bound still terminates. Plain aggregate — no secrets logic.
 */
struct ProvisioningRecord {
    char ssid[kMaxSsidLen + 1];
    char pass[kMaxPassLen + 1];
    char key[kMaxKeyLen + 1];
    /// Optional push-notification endpoint (ADR-0023). Empty when push is not configured; validated
    /// separately by isUsableWebhookUrl(), so a bad value disables push without gating the boot triad.
    char webhookUrl[kMaxWebhookUrlLen + 1];
    /// Whether the operator has *armed* autonomous deauth (ADR-0029). Default **false**: the raw-TX
    /// capability ships in every binary, but the hunt loop transmits deauth only when this is set, so
    /// a fresh, upgraded, or unarmed device never jams. Set from the captive-portal checkbox; absent
    /// from NVS on a device provisioned before it existed, where getBool() reads it back as false.
    bool deauthEnabled;
};

/**
 * @brief Load the stored triad into @p out and report whether it is usable.
 *
 * Returns true only when all three fields were present in NVS *and* pass validateCredentials()
 * — a half-written or corrupt record reads as "not provisioned" so the boot gate opens the
 * portal rather than trying to associate with junk. @p out is fully overwritten either way (a
 * zeroed record on false), so a caller never reads a stale stack buffer.
 */
bool loadProvisioning(ProvisioningRecord& out);

/**
 * @brief Validate @p record and, only if usable, write all three fields to NVS. The sole writer.
 *
 * Returns false without touching NVS when the triad fails validateCredentials(), so an invalid
 * form submission can never overwrite a good stored record (fail loud, no partial write). On a
 * valid record every field is written, so the store is always a complete triad or the previous
 * one — never a mix.
 */
bool persistProvisioning(const ProvisioningRecord& record);

/**
 * @brief Erase the stored triad, forcing the next boot into the portal (re-provisioning).
 */
void clearProvisioning();

/**
 * @brief Set the stored triad to exactly @p seed: persist it if valid, otherwise clear any triad
 *        already in NVS. Composes persistProvisioning()/clearProvisioning() (no direct NVS access),
 *        so the store stays a single-writer seam (invariant #6).
 *
 * Used by the compile-time `SAPPER_TEST_HOOKS` seed (main.cpp): a hooks build with no credential
 * flags seeds an empty (invalid) triad, and clearing on that failure makes the store
 * deterministically reflect the build's OWN flags — an empty seed routes to the portal instead of
 * silently inheriting credentials a prior flash left in NVS (ADR-0008, fail loud).
 */
void seedProvisioning(const ProvisioningRecord& seed);

}  // namespace sapper
