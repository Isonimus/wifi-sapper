/**
 * @file submitted_store_littlefs.h
 * @brief LittleFS-on-internal-flash backing for the SubmittedStore seam (ADR-0065 gate C). Device-only.
 *
 * The local twin of LittleFsCrackedStore. The submitted set is one small map read and rewritten whole:
 * all entries in a single file `/submitted.bin`, loaded on boot, persisted atomically as a complete set
 * via a temp-file-then-rename write (`/submitted.tmp` → `/submitted.bin`, never in-place), so a reader
 * never sees a half-written set and a power loss mid-write leaves the old file ready to read.
 *
 * Every operation fails loud (quality bar §3): a mount, read, write, or rename failure is reported to
 * the caller and never swallowed into a silently-lost set.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/submitted_store.h"

namespace sapper {

class LittleFsSubmittedStore : public SubmittedStore {
public:
    /// @param basePath the file stem for this set's two backing files: "<basePath>.bin" (the set) and
    /// "<basePath>.tmp" (the atomic-rename staging file). Defaults to "/submitted" so the single-backend
    /// store is byte-for-byte unchanged (ADR-0065). ADR-0109's pwncrack second backend passes a DISTINCT
    /// stem (§4 #46c: "its own SubmittedStore namespace") so the two per-backend submitted sets never
    /// collide on the shared filesystem. The stem must fit kBasePathCap below (both current stems do).
    explicit LittleFsSubmittedStore(const char* basePath = "/submitted");

    /// Mount LittleFS (formatting a blank/corrupt partition once). Returns false if the filesystem could
    /// not be mounted — the caller must treat the store as unavailable and fail loud. Idempotent:
    /// LittleFS.begin() may already have mounted the same filesystem this run (capture/cracked stores).
    bool begin();

    bool load(SubmittedEntry* out, size_t capacity, size_t& count) override;
    bool save(const SubmittedEntry* entries, size_t count) override;

private:
    /// Cap for a backing-file path ("<basePath>.bin"): room for a ~26-char stem plus the ".bin"/".tmp"
    /// suffix and NUL, well over both shipped stems ("/submitted", "/pwncrack_sub"). The constructor
    /// fails loud (a one-line [ERROR] and an empty path that makes begin()/load()/save() fail) rather
    /// than silently truncate a stem that overruns it (quality bar §3).
    static constexpr size_t kBasePathCap = 32;
    char binPath_[kBasePathCap] = {};
    char tmpPath_[kBasePathCap] = {};
    bool pathValid_ = false;  ///< False if the stem overran kBasePathCap; begin() then fails loud (§3).
    bool mounted_ = false;
};

}  // namespace sapper

#endif  // UNIT_TEST
