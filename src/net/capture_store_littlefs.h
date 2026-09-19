/**
 * @file capture_store_littlefs.h
 * @brief LittleFS-on-internal-flash backing for the CaptureStore seam (ADR-0017 decision #3).
 *        Device-only.
 *
 * The portable floor: LittleFS is present on every ESP32 target and survives reset, so a queue of
 * captures taken offline outlives the routine power loss of a battery appliance without an SD card
 * (which ADR-0001's headless-first promise forbids requiring). One pcap per pending capture is stored
 * as a file whose name encodes both the store-assigned id and the BSSID —
 * `<8-hex-id>_<12-hex-bssid>.pcap` — so listPending() reconstructs the pending set from directory
 * names alone, without reading any file, and ids resume above the highest on flash at begin() so a
 * reboot never reissues one (the contract capture_store.h depends on for oldest-eviction).
 *
 * Every operation fails loud (quality bar §3, ADR-0017 decision #4): a mount, write, read, or delete
 * failure is reported to the caller, never swallowed into a silently-lost capture.
 */
#pragma once

#ifndef UNIT_TEST

#include "net/capture_store.h"

namespace sapper {

class LittleFsCaptureStore : public CaptureStore {
public:
    /// Mount LittleFS (formatting a blank/corrupt partition once) and resume the id counter above the
    /// highest id already on flash. Returns false if the filesystem could not be mounted — the caller
    /// must treat the store as unavailable and fail loud, not run a queue with nowhere to persist.
    bool begin();

    bool enqueue(const uint8_t bssid[6], const uint8_t* pcap, size_t len, uint32_t& outId) override;
    bool listPending(PendingCapture* out, size_t capacity, size_t& count) override;
    bool read(uint32_t id, uint8_t* out, size_t capacity, size_t& len) override;
    bool remove(uint32_t id) override;

private:
    /// Write the on-flash path for @p id + @p bssid into @p out; returns false if @p out is too small.
    bool formatPath(char* out, size_t cap, uint32_t id, const uint8_t bssid[6]) const;
    /// Find the stored file whose name carries @p id, writing its full path into @p out. False if none.
    bool findPathById(char* out, size_t cap, uint32_t id) const;

    bool mounted_ = false;
    uint32_t nextId_ = 1;  // 1-based so 0 stays a "none" sentinel, matching the fake store.
};

}  // namespace sapper

#endif  // UNIT_TEST
