/**
 * @file wpasec_response.h
 * @brief Pure, host-tested classification of wpa-sec's upload response (ADR-0017 decision #2, ADR-0076).
 *
 * wpa-sec replies with hcxpcapngtool's verbose summary. The success signal is not the presence of a
 * label — hcx always prints "EAPOL pairs written to 22000 hash file...: N" and "PMKID(s) written to
 * 22000 hash file...: N" whether N is 0 or not — but the **count**. Classifying on the label alone
 * false-accepts a 0-result upload (fail-open: deleting a capture wpa-sec extracted nothing from). So
 * this parses the number after any "written to 22000 hash file" line and accepts only when a count is
 * >= 1; a duplicate is terminal success; everything else (including "No valid handshakes/PMKIDs
 * found" and all-zero counts) is Rejected — kept and retried, never assumed success (quality bar §3).
 *
 * The classifier is **streaming** (ADR-0076): it consumes the reply in arbitrary chunks and reaches its
 * verdict with O(1) state, holding no buffer sized to the response. This is why: on the no-PSRAM
 * ESP32-S3 the former whole-reply `new char[16384]` failed under TLS memory pressure (largest contiguous
 * block measured at 7668 B), misclassifying every reply Rejected so no capture was ever marked submitted.
 * Streaming removes the allocation. The device wrapper (uploader_wpasec.cpp) reads the TLS bytes into a
 * small fixed stack buffer and feeds them here; the logic carries no protocol-transport concern.
 *
 * Hardware-free so it is unit-tested against real captured wpa-sec responses on the native lane
 * (test/test_wpasec_response, ADR-0004 lane 1), including every chunk-boundary split (a verdict must not
 * depend on how TLS happened to split the bytes).
 */
#pragma once

#include <cstddef>

#include "net/uploader.h"

namespace sapper {
namespace detail {

/**
 * Streaming KMP substring matcher over one fixed token (the token must outlive the matcher). Correct for
 * **any** token, including one whose prefix recurs within itself (e.g. "already in database", where the
 * repeated 'a' makes a naive first-char restart silently miss a real occurrence that follows a partial
 * match). O(token) state, O(1) amortized per byte. `feed` returns true when the token completes.
 */
struct TokenMatcher {
    void init(const char* token);  ///< set the token and build its KMP failure table.
    bool feed(char c);             ///< true when the token completes at this byte.
    void reset() { matched = 0; }

    /// Fixed cap for the failure table; the two call-site tokens are compile-time literals well under it
    /// (asserted at the ctor call sites). It bounds the per-matcher state so nothing scales with the reply.
    static constexpr size_t kMaxToken = 32;
    const char* token = nullptr;
    size_t len = 0;
    size_t failure[kMaxToken] = {};
    size_t matched = 0;
};

}  // namespace detail

/**
 * Streaming classifier for a wpa-sec upload reply. Feed the response body in any number of chunks of any
 * size (`feed`), then read the verdict (`result`). O(1) state: two fixed-token matchers, a tiny per-line
 * count state machine, and a bounded integer accumulator — no buffer proportional to the reply.
 *
 * Contract (identical to the whole-string form, ADR-0017 #2): Duplicate on "already in database"
 * (outranks Accepted when both appear, in any order), Accepted on a "written to 22000 hash file" line
 * whose count parses >= 1, everything else Rejected. Duplicate is terminal (`terminal()` true once seen,
 * so a reader may stop early); Accepted is only latched, never terminal, so a later dedup line still wins.
 */
class WpaSecResponseClassifier {
public:
    WpaSecResponseClassifier();

    /// Consume the next @p len bytes of the response body. Safe across arbitrary chunk boundaries.
    void feed(const char* data, size_t len);

    /// The verdict given everything fed so far (Duplicate > Accepted > Rejected).
    UploadResult result() const;

    /// True once a terminal verdict (Duplicate) is reached — the reader may stop consuming.
    bool terminal() const { return duplicate_; }

private:
    void feedChar(char c);

    /// Where the per-line written-count scan is: matching the label, skipping to the first digit on the
    /// line after it, or accumulating that digit run.
    enum class WrittenState { Scanning, AfterLabel, ReadingCount };

    detail::TokenMatcher dupMatcher_;      ///< matches "already in database" anywhere (outranks, terminal).
    detail::TokenMatcher writtenMatcher_;  ///< matches "written to 22000 hash file" to open a count scan.
    WrittenState writtenState_ = WrittenState::Scanning;
    unsigned countValue_ = 0;              ///< the digit run after a matched label (bounded; only >= 1 matters).
    bool duplicate_ = false;
    bool accepted_ = false;
};

/// Classify a complete wpa-sec response body (NUL-terminated). A thin wrapper over the streaming
/// classifier (one implementation, DRY): feeds the whole string once and returns the verdict.
UploadResult classifyWpaSecResponse(const char* response);

}  // namespace sapper
