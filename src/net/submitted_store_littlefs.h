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
    /// Mount LittleFS (formatting a blank/corrupt partition once). Returns false if the filesystem could
    /// not be mounted — the caller must treat the store as unavailable and fail loud. Idempotent:
    /// LittleFS.begin() may already have mounted the same filesystem this run (capture/cracked stores).
    bool begin();

    bool load(SubmittedEntry* out, size_t capacity, size_t& count) override;
    bool save(const SubmittedEntry* entries, size_t count) override;

private:
    bool mounted_ = false;
};

}  // namespace sapper

#endif  // UNIT_TEST
