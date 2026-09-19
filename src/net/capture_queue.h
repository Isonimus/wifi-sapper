/**
 * @file capture_queue.h
 * @brief The bounded, per-BSSID-deduplicated retry-queue policy over a CaptureStore (ADR-0017
 *        decision #4).
 *
 * Pure policy: it owns no bytes and no filesystem. It serializes a captured handshake to a pcap
 * (net/pcap.h), then decides — from the store's own pending list — whether that pcap is a new entry,
 * a replacement for a capture already queued for the same BSSID, or an eviction that pushes the
 * oldest out to stay within the bound, and drives the abstract CaptureStore accordingly. The device
 * backs the store with LittleFS and a host test backs it with an in-RAM fake, so the whole policy is
 * unit-tested on the native lane (ADR-0004 lane 1; slice-0018 Scenario E).
 *
 * The store is the single source of truth, including across a reboot, so the queue keeps no parallel
 * index: every decision is read from CaptureStore::listPending() (oldest-first). The queue is also
 * the store's single writer — the drain supervisor reaches the store only through pending()/read()/
 * remove() here — so "one module opens or writes captures" stays checkable (§4 invariant #9 spirit).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/capture_store.h"
#include "net/handshake_collector.h"
#include "net/pcap.h"

namespace sapper {

/// The most pending captures the queue keeps on flash. A hard policy bound (ADR-0017 decision #4):
/// an endlessly-offline appliance must not grow its flash use without limit. At kMaxSerializedPcapLen
/// each, this is well under a LittleFS partition, so the *count* bound is reached long before flash
/// fills — flash-full is then a distinct, genuine failure the store surfaces loud.
constexpr size_t kMaxQueuedCaptures = 32;

/// How many pending entries listPending() may report before the queue treats it as a fault. Sized
/// above kMaxQueuedCaptures so a transient over-bound left by a failed reconcile (see offer()) has
/// room to be read and healed rather than wedging the queue; a store somehow holding more than this
/// is a corruption to surface, not to silently truncate (quality bar §3).
constexpr size_t kCaptureStoreScanCapacity = 2 * kMaxQueuedCaptures;

/// What offer() did with a capture, so the supervisor can log it precisely. StoreError means the
/// store failed loud (quality bar §3): either the capture could not be persisted at all, or it was
/// persisted but the store could not be reconciled to the bound/dedup invariant afterwards.
enum class OfferResult { Stored, Replaced, Evicted, NotUploadable, StoreError };

/**
 * @brief Applies the bound + per-BSSID-dedup + oldest-eviction policy over an injected CaptureStore.
 *
 * Construct with the store; call offer() for each capture-ready handshake, and pending()/read()/
 * remove() to drive a drain. Holds one scratch buffer for pcap serialization; no dynamic allocation.
 */
class CaptureQueue {
public:
    explicit CaptureQueue(CaptureStore& store) : store_(store) {}

    /**
     * @brief Serialize @p handshake to a pcap and enqueue it under the policy.
     *
     * NotUploadable if the handshake is not wpa-sec-valid (nothing to store). Otherwise the new pcap
     * is persisted **first** — so a store failure never discards the freshest, most-complete capture
     * in hand — and only then is the store reconciled: every *other* pending entry for the same BSSID
     * is removed (a later capture holds at least as much of M1-M4, ADR-0017 decision #4), and if the
     * store is over the bound the oldest entries are evicted. Returns Replaced/Evicted to report
     * which reconcile happened, Stored for a plain new BSSID under the bound, or StoreError if the
     * persist failed or a reconcile remove failed (the new capture still stands in the latter case;
     * the failure is surfaced so the supervisor logs it, never swallowed).
     */
    OfferResult offer(const CapturedHandshake& handshake);

    /// Enumerate pending captures oldest-first for a drain. Returns false on a store error.
    bool pending(PendingCapture* out, size_t capacity, size_t& count);

    /// Read one pending capture's pcap bytes for upload. Returns false if absent or too large.
    bool read(uint32_t id, uint8_t* out, size_t capacity, size_t& len);

    /// Remove a pending capture — called only after a terminal accepted/duplicate upload. Returns
    /// false if it was absent or the delete failed.
    bool remove(uint32_t id);

private:
    CaptureStore& store_;
    uint8_t scratch_[kMaxSerializedPcapLen];  ///< serialize target; reused per offer(), no heap.
};

}  // namespace sapper
