/**
 * @file cracked_store_littlefs.h
 * @brief LittleFS-on-internal-flash backing for the CrackedStore seam (ADR-0019 decision #4).
 *        Device-only.
 *
 * The portable floor: LittleFS is present on every ESP32 target and survives reset, so the
 * account mirror outlives the routine power loss of a battery appliance. Unlike the CaptureStore
 * (a set of independently-added/removed pcap blobs), the cracked manifest is one small map
 * read and rewritten whole on each successful sync: all entries are stored in a single file
 * `/cracked.bin`, loaded into memory on boot, and persisted atomically as a complete manifest.
 * This keeps it simple — the entire account state is one transaction — and bounded — the map
 * size is known at design time (ADR-0019 decision #4). A temp-file-then-rename write (always
 * `/cracked.tmp` → `/cracked.bin`, never in-place) guarantees that a reader never sees a
 * half-written manifest: the atomic filesystem rename ensures the prior manifest stays intact
 * until the new one is complete, so a power loss mid-write leaves the old file ready to read.
 *
 * Every operation fails loud (quality bar §3, ADR-0019): a mount, read, write, or rename
 * failure is reported to the caller and never swallowed into a silently-lost account.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/cracked_store.h"

namespace sapper {

class LittleFsCrackedStore : public CrackedStore {
public:
    /// Mount LittleFS (formatting a blank/corrupt partition once). Returns false if the
    /// filesystem could not be mounted — the caller must treat the store as unavailable and
    /// fail loud, not run a sync with nowhere to persist. On success, the store is ready to
    /// load or save the manifest.
    bool begin();

    bool load(CrackedEntry* out, size_t capacity, size_t& count, bool& freshManifest) override;
    bool save(const CrackedEntry* entries, size_t count, bool freshManifest) override;

private:
    bool mounted_ = false;
};

}  // namespace sapper

#endif  // UNIT_TEST
