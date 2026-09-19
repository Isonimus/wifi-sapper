/**
 * @file capture_queue.cpp
 * @brief Implementation of the bounded, per-BSSID-deduplicated retry-queue policy (ADR-0017).
 */
#include "net/capture_queue.h"

#include <cstring>

namespace sapper {
namespace {

/// A CaptureSink over a fixed caller-owned buffer. serializeHandshake writes at most
/// kMaxSerializedPcapLen bytes, so a buffer of that size can never overrun; a write past the end is
/// still rejected (returns false) rather than truncated, so a size-accounting bug fails loud
/// (quality bar §3) instead of emitting a silently corrupt pcap.
class BufferCaptureSink : public CaptureSink {
public:
    BufferCaptureSink(uint8_t* buffer, size_t capacity) : buffer_(buffer), capacity_(capacity) {}

    bool write(const uint8_t* data, size_t len) override {
        if (written_ + len > capacity_) return false;
        std::memcpy(buffer_ + written_, data, len);
        written_ += len;
        return true;
    }

    size_t written() const { return written_; }

private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t written_ = 0;
};

}  // namespace

OfferResult CaptureQueue::offer(const CapturedHandshake& handshake) {
    // Serialize first; an invalid handshake yields no pcap, so there is nothing to store. The engine
    // only reports valid captures, but we re-check rather than trust the caller (fail loud).
    BufferCaptureSink sink(scratch_, sizeof(scratch_));
    if (!serializeHandshake(handshake, sink)) return OfferResult::NotUploadable;

    // Persist the freshest capture *first* (ADR-0017 decision #4): a store failure must never cost us
    // the most complete capture we hold. Everything after this is best-effort reconcile — the bytes are
    // already safe on flash, so a later failure surfaces StoreError without ever discarding the capture.
    uint32_t newId = 0;
    if (!store_.enqueue(handshake.bssid, scratch_, sink.written(), newId)) return OfferResult::StoreError;

    // Snapshot the queue for reconcile. It now *includes* the just-stored entry (the highest id), so we
    // exclude it by id below. If the listing itself fails the capture still stands: reconcile is skipped
    // this call and a later offer() heals any leftover duplicate/overshoot (the dedup loop removes *all*
    // same-BSSID matches for exactly that reason), so we return StoreError without losing bytes.
    PendingCapture snapshot[kCaptureStoreScanCapacity];
    size_t snapshotCount = 0;
    if (!store_.listPending(snapshot, kCaptureStoreScanCapacity, snapshotCount)) {
        return OfferResult::StoreError;
    }

    bool reconcileFailed = false;
    bool replaced = false;

    // Dedup: drop every *other* entry for this BSSID, keeping only the just-stored one. Removing all
    // matches (not just one) also heals any duplicates a previous failed reconcile left behind.
    size_t survivorCount = 0;
    PendingCapture survivors[kCaptureStoreScanCapacity];
    for (size_t i = 0; i < snapshotCount; ++i) {
        if (snapshot[i].id == newId) continue;  // the just-stored capture — never reconcile it away.
        if (std::memcmp(snapshot[i].bssid, handshake.bssid, 6) == 0) {
            replaced = true;
            if (!store_.remove(snapshot[i].id)) reconcileFailed = true;
        } else {
            survivors[survivorCount++] = snapshot[i];  // still pending, distinct BSSID; oldest-first.
        }
    }

    // Eviction: the just-stored capture plus the distinct-BSSID survivors must fit the bound. Evict
    // oldest survivors (front of the ascending list) until they do. Enqueue-first means we can sit at
    // bound+1 for the span of this call; a small transient overshoot on flash, corrected here.
    bool evicted = false;
    size_t evictAt = 0;
    while (survivorCount - evictAt + 1 > kMaxQueuedCaptures) {
        evicted = true;
        if (!store_.remove(survivors[evictAt].id)) reconcileFailed = true;
        ++evictAt;
    }

    if (reconcileFailed) return OfferResult::StoreError;  // new capture stands; tidy-up failed loud.
    if (evicted) return OfferResult::Evicted;
    if (replaced) return OfferResult::Replaced;
    return OfferResult::Stored;
}

bool CaptureQueue::pending(PendingCapture* out, size_t capacity, size_t& count) {
    return store_.listPending(out, capacity, count);
}

bool CaptureQueue::read(uint32_t id, uint8_t* out, size_t capacity, size_t& len) {
    return store_.read(id, out, capacity, len);
}

bool CaptureQueue::remove(uint32_t id) { return store_.remove(id); }

}  // namespace sapper
