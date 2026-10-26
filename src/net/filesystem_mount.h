/**
 * @file filesystem_mount.h
 * @brief The single LittleFS mount seam: loud + breadcrumbed on failure, never a silent reformat
 *        (ADR-0074, §4 #34).
 *
 * Every store used to mount with `LittleFS.begin(formatOnFail=true)`, which returns true even when
 * it had to reformat a corrupt partition, so a transient corruption silently wiped a unit's
 * persisted store with no signal (a field run destroyed a real store this way). This seam is the one
 * place that calls `LittleFS.begin`: it tries a non-formatting mount first, and only on failure logs
 * one loud `[ERROR]`, records a durable NVS reformat-count breadcrumb, and reformats. The decision is
 * the pure `decideMountOutcome` below (host-tested); the Arduino glue (`LittleFS.begin`, NVS) is
 * device-only in the .cpp, like all store byte-format and NVS code.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// The result of a mount attempt through the seam (ADR-0074 decision 1).
enum class MountOutcome {
    Mounted,      ///< Mounted cleanly, nothing lost.
    FirstFormat,  ///< A blank/never-initialised partition formatted for the first time: nothing to lose.
    Reformatted,  ///< A partition that previously held a filesystem failed to mount and was reformatted: data lost.
    Failed,       ///< Even the reformat failed: there is no filesystem.
};

/// The pure mount decision (ADR-0074 decision 2). Kept distinct from the Arduino glue so the point the
/// silent-reformat defect lost (that a reformat is its own outcome, not folded into a plain success) is
/// host-tested. A non-formatting mount that succeeds is `Mounted`; if it fails, a successful formatting
/// mount is `Reformatted` when the partition had a filesystem before (@p hadFilesystemBefore: the loud +
/// breadcrumb data-loss path) or `FirstFormat` when it did not (a blank or erased partition's first init,
/// which is expected and silent). A field re-test showed an erased partition reports the identical
/// `Corrupted dir pair` mount failure as a corrupt one, so without this flag every fresh device would
/// false-alarm "data lost". Both mounts failing is `Failed`.
inline MountOutcome decideMountOutcome(bool firstMountOk, bool reformatMountOk, bool hadFilesystemBefore) {
    if (firstMountOk) return MountOutcome::Mounted;
    if (!reformatMountOk) return MountOutcome::Failed;
    return hadFilesystemBefore ? MountOutcome::Reformatted : MountOutcome::FirstFormat;
}

/// Mount the LittleFS data partition through the policy above (device-only). On a corrupt partition
/// it logs one loud `[ERROR]` and increments the persisted reformat breadcrumb before reformatting.
/// Idempotent within a boot: once mounted, later calls see it mounted and neither log nor breadcrumb,
/// so the three stores and the rssi probe can each call it while only the first does the work.
MountOutcome ensureFilesystemMounted();

/// The durable reformat-count breadcrumb (ADR-0074 decision 3), read from NVS (device-only). Zero on
/// a healthy unit; a rising value across Maintenance visits is the field signal of recurring flash
/// corruption. Lives in its own NVS namespace, so it survives both the LittleFS format it records and
/// a provisioning clear().
uint32_t filesystemReformatCount();

}  // namespace sapper
