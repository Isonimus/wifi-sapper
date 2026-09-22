/**
 * @file submitted_lookup.h
 * @brief The read + record seams for the "already submitted to wpa-sec?" set (ADR-0065 gate C).
 *
 * The general form of the cracked seams (net/cracked_lookup.h): where CrackedLookup answers "have we
 * *recovered* this BSSID?", these answer "have we *landed a valid handshake* for it at wpa-sec?" — the
 * earlier, larger fact (cracked ⊆ submitted). Two seams, mirroring how the supervisor writes and the
 * engine only reads:
 *
 *  - SubmittedLookup — read only (isSubmitted). The HuntEngine consumes this in target selection
 *    (gate C's targeting skip), coupling to the *question*, not to SubmittedManifest's concrete type,
 *    so it stays host-testable against a trivial fake (exactly as gate B uses CrackedLookup).
 *  - SubmittedRecorder — the write-capable extension the UploadSupervisor consumes: it records a
 *    submission on a terminal upload success and drops an already-submitted queued capture, so it needs
 *    both to read and to mark/flush.
 *
 * SubmittedManifest implements SubmittedRecorder over its bounded per-BSSID set (submitted_manifest.h),
 * so neither gate grows a parallel parse of the store.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// Answers "has this appliance already landed a valid handshake for this BSSID at wpa-sec?" — submitted
/// membership, read only. The engine's gate-C target filter (ADR-0065 #2).
struct SubmittedLookup {
    virtual ~SubmittedLookup() = default;
    virtual bool isSubmitted(const uint8_t bssid[6]) const = 0;
};

/// The write-capable seam the UploadSupervisor consumes (ADR-0065 #3): read (drop an already-submitted
/// queued capture) plus record (mark a BSSID submitted on a terminal Accepted/Duplicate) and persist
/// (one flush per drain cycle). Extends SubmittedLookup so the drain reads through the one pointer.
struct SubmittedRecorder : SubmittedLookup {
    /// Record that a valid handshake for @p bssid reached wpa-sec. RAM-only and idempotent (re-marking a
    /// held BSSID keeps its original uploadedAt); persisted by the next flushSubmitted(). @p nowMs is the
    /// eviction key for oldest-out at the bound.
    virtual void markSubmitted(const uint8_t bssid[6], uint32_t nowMs) = 0;

    /// Persist the accumulated marks as one whole set. Returns false on a store error (fail loud — §3);
    /// the caller counts it but does not fail the upload cycle (ADR-0065 #3: the uploads already landed).
    virtual bool flushSubmitted() = 0;
};

}  // namespace sapper
