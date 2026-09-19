/**
 * @file capture_store.h
 * @brief The persistence seam under the retry queue: where pending pcaps wait for upload (ADR-0017
 *        decision #3).
 *
 * The pure CaptureQueue policy (capture_queue.h) talks only to this abstract store; it never opens a
 * file. The device backs it with LittleFS on internal flash (capture_store_littlefs.h) so a queue
 * survives the routine power loss of a portable appliance, and a host test backs it with an in-RAM
 * fake (test/support/fake_capture_store.h) — exactly the seam split ADR-0004 lane 1 uses for the
 * radio. This mirrors the CaptureSink seam pcap.h already leaves open: CaptureSink is the byte-stream
 * a single pcap is serialized into; CaptureStore is the set of whole pcaps waiting to go.
 *
 * The store is the single source of truth for what is pending — including across a reboot — so the
 * queue holds no parallel index to fall out of sync with it. Two contracts make that safe:
 *   - ids are monotonically increasing, and survive a reboot: a store that resumes from flash never
 *     reissues an id it handed out before, so "the oldest pending" is unambiguously the lowest id;
 *   - listPending() returns entries oldest-first (ascending id), so the queue's per-BSSID dedup and
 *     oldest-eviction are decided from the list alone (ADR-0017 decision #4).
 *
 * Every operation reports failure rather than swallowing it: a full, corrupt, or absent flash must
 * surface as an error the drain loop counts and logs, never a capture silently reported stored or
 * uploaded that was not (quality bar §3, ADR-0017 decision #4).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// Identity of one pending capture, as the store enumerates it. The bytes stay in the store; the
/// queue reasons over this metadata (which BSSID, which is oldest) without reading pcaps back.
struct PendingCapture {
    uint32_t id = 0;         ///< Store-assigned, monotonically increasing; lower == older.
    uint8_t bssid[6] = {0};  ///< The AP this capture is for — the per-BSSID dedup key.
};

/// Where pending pcaps persist between capture and a successful upload. Backed by LittleFS on-device
/// and an in-RAM fake on the host lane. All methods fail loud (quality bar §3).
class CaptureStore {
public:
    virtual ~CaptureStore() = default;

    /// Persist @p len bytes of pcap for @p bssid under a freshly assigned id, returned in @p outId.
    /// Returns false on any store failure (full, write error) having stored nothing — the caller
    /// keeps the capture in hand and fails loud, never assumes it was saved.
    virtual bool enqueue(const uint8_t bssid[6], const uint8_t* pcap, size_t len, uint32_t& outId) = 0;

    /// Fill @p out (capacity @p capacity entries) with every pending capture, oldest-first, and set
    /// @p count. Returns false on a store error, or if more entries are pending than @p capacity can
    /// hold (an overrun is a fault to surface, not a set to silently truncate — quality bar §3).
    virtual bool listPending(PendingCapture* out, size_t capacity, size_t& count) = 0;

    /// Read the pcap bytes for @p id into @p out (capacity @p capacity), setting @p len. Returns
    /// false if @p id is absent, its bytes exceed @p capacity, or the read fails — never a partial
    /// or truncated pcap.
    virtual bool read(uint32_t id, uint8_t* out, size_t capacity, size_t& len) = 0;

    /// Delete the pending capture @p id. Returns false if it was absent or the delete failed.
    virtual bool remove(uint32_t id) = 0;
};

}  // namespace sapper
