/**
 * @file filesystem_mount.cpp
 * @brief Device-only glue for the LittleFS mount seam (ADR-0074). Arduino LittleFS + NVS; the pure
 *        decision it wraps (decideMountOutcome) lives in the header and is host-tested.
 */
#include "net/filesystem_mount.h"

#ifndef UNIT_TEST

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

namespace sapper {
namespace {

// The breadcrumb lives in its own NVS namespace, deliberately NOT the `sapper` provisioning one: NVS
// is a separate partition, so the count survives both the LittleFS format it records and a
// provisioning clear() (which wipes the whole `sapper` namespace, ADR-0006). Both names are within
// NVS's 15-char limit.
constexpr char kFsHealthNamespace[] = "fshealth";
constexpr char kKeyReformats[] = "reformats";
// Set true the first time a filesystem is successfully mounted on this unit (ADR-0074 decision 3). It is
// how a later mount failure tells a real reformat (this was set, so the partition had a filesystem, so
// data was lost: loud + breadcrumb) from a blank partition's first-ever format (unset, nothing to lose:
// silent). Without it, an erased or new partition, which reports the identical `Corrupted dir pair` mount
// failure as a corrupt one, would false-alarm "data lost" on every fresh device and emit an [ERROR] that
// trips the verify gates on first boot.
constexpr char kKeySeen[] = "fs_seen";

/// Increment the durable reformat count (ADR-0074 decision 3). A failed NVS write is itself loud but
/// not fatal: the reformat already happened, so the appliance keeps running; only the breadcrumb is
/// lost (fail loud, never swallow; quality bar §3).
void recordReformat() {
    Preferences prefs;
    if (!prefs.begin(kFsHealthNamespace, /*readOnly=*/false)) {
        Serial.println("[ERROR] fs-health NVS open failed: reformat not counted");
        return;
    }
    const uint32_t next = prefs.getUInt(kKeyReformats, 0) + 1;
    const bool wrote = prefs.putUInt(kKeyReformats, next) == sizeof(uint32_t);
    prefs.end();
    if (wrote) {
        Serial.printf("[ERROR] LittleFS reformat breadcrumb count=%lu\n", static_cast<unsigned long>(next));
    } else {
        Serial.println("[ERROR] fs-health breadcrumb write failed: reformat not counted");
    }
}

/// Whether a filesystem has ever been successfully mounted on this unit (ADR-0074 decision 3). A missing
/// namespace reads false: a brand-new device that has never mounted anything, which is exactly the
/// blank-partition first-format case that must stay silent.
bool filesystemWasSeen() {
    Preferences prefs;
    if (!prefs.begin(kFsHealthNamespace, /*readOnly=*/true)) return false;
    const bool seen = prefs.getBool(kKeySeen, false);
    prefs.end();
    return seen;
}

/// Record, once, that a filesystem has been successfully mounted. Read-only first so the one-time write
/// never recurs (no per-boot flash write); a failure to persist the marker is non-fatal (the next boot
/// retries), like the breadcrumb write.
void markFilesystemSeen() {
    if (filesystemWasSeen()) return;
    Preferences prefs;
    if (!prefs.begin(kFsHealthNamespace, /*readOnly=*/false)) {
        // Fail loud like recordReformat() and provisioning_store (§3): a swallowed failure here is worse
        // than a lost breadcrumb. If the marker never persists, a later real reformat is forever
        // misclassified as a silent FirstFormat, the very data-loss ADR-0074 kills. A persistent [ERROR]
        // every boot is the signal that this unit's classification can no longer be trusted.
        Serial.println("[ERROR] fs-health NVS open failed: filesystem-seen marker not set");
        return;
    }
    const bool wrote = prefs.putBool(kKeySeen, true) == sizeof(uint8_t);
    prefs.end();
    if (!wrote) {
        Serial.println("[ERROR] fs-health seen-marker write failed: a later reformat may be misclassified");
    }
}

}  // namespace

MountOutcome ensureFilesystemMounted() {
    // Try a NON-formatting mount first: this is the whole fix. The old formatOnFail=true reformatted a
    // corrupt partition and still returned true, so the loss was silent. A clean (or already-mounted)
    // partition returns Mounted, silent, and records that a filesystem exists here so a FUTURE mount
    // failure is known to be a real reformat, not a first-time format.
    if (LittleFS.begin(/*formatOnFail=*/false)) {
        markFilesystemSeen();
        return MountOutcome::Mounted;
    }

    // The non-formatting mount failed. Decide what that means BEFORE formatting: a partition we have
    // mounted before is now corrupt (real data loss), whereas one we never mounted is simply blank or
    // erased (nothing to lose). A field re-test showed an erased partition reports the identical
    // `Corrupted dir pair` failure as a corrupt one, so this seen-flag is the only way to tell them
    // apart (ADR-0074 decision 3); without it, every fresh device would false-alarm.
    const bool hadFilesystemBefore = filesystemWasSeen();
    const bool reformatMountOk = LittleFS.begin(/*formatOnFail=*/true);
    const MountOutcome outcome =
        decideMountOutcome(/*firstMountOk=*/false, reformatMountOk, hadFilesystemBefore);
    switch (outcome) {
        case MountOutcome::Reformatted:
            // A filesystem that existed was lost: loud + counted, never silent (the whole point of #34),
            // and caught by the verify gates (which match ^[ERROR]).
            Serial.println("[ERROR] LittleFS mount failed (corrupt): reformatted, persisted data lost");
            recordReformat();
            markFilesystemSeen();
            break;
        case MountOutcome::FirstFormat:
            // A blank/erased partition initialised for the first time: expected, nothing lost, so NOT an
            // [ERROR]; it must not trip the verify gates on a fresh device's first boot.
            Serial.println("[FS] first-time format of a new or erased partition (no prior data)");
            markFilesystemSeen();
            break;
        case MountOutcome::Failed:
            // The reformat itself failed, so there is no filesystem. The caller logs its own [FATAL]
            // with store-specific context and halts (hunt_loop); this seam does not duplicate it.
            break;
        case MountOutcome::Mounted:
            break;  // unreachable: a successful non-formatting mount returned above.
    }
    return outcome;
}

uint32_t filesystemReformatCount() {
    Preferences prefs;
    // Read-only begin returns false when the namespace was never created: a unit that has never
    // reformatted, i.e. a count of zero, not an error.
    if (!prefs.begin(kFsHealthNamespace, /*readOnly=*/true)) return 0;
    const uint32_t count = prefs.getUInt(kKeyReformats, 0);
    prefs.end();
    return count;
}

}  // namespace sapper

#endif  // UNIT_TEST
