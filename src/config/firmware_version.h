/**
 * @file firmware_version.h
 * @brief The single source of truth for the firmware version string (ADR-0041 decision 4).
 *
 * Shown on the boot splash (slice-0042) and available to any later surface that reports what is
 * flashed (a serial `state` line, the Maintenance dashboard). Bump this on a release — one place,
 * so no surface can drift from another. Not a magic string in the boot path.
 */
#pragma once

namespace sapper {

/// Semantic version of the running firmware. `0.1.0` is the pre-release appliance's first tagged face.
constexpr char kFirmwareVersion[] = "0.1.0";

}  // namespace sapper
