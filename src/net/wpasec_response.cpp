/**
 * @file wpasec_response.cpp
 * @brief Implementation of the streaming wpa-sec response classifier (ADR-0017 decision #2, ADR-0076).
 */
#include "net/wpasec_response.h"

#include <cstring>

namespace sapper {
namespace {

// wpa-sec already holds this capture — terminal success, off our hands (ADR-0017 decision #2). Outranks a
// positive written count in the same body, so it is matched on every byte and makes the verdict terminal.
constexpr char kDuplicate[] = "already in database";

// hcx prints both a PMKID and an EAPOL "written to 22000 hash file" line; matching this shared tail and
// reading the count after it covers both without depending on the exact prefix or the "(s)".
constexpr char kWrittenLabel[] = "written to 22000 hash file";

// Both tokens are fixed literals and must fit the matcher's failure table. Fail loud at compile time if a
// future edit grows one past the bound, rather than silently truncating it (quality bar §3).
static_assert(sizeof(kDuplicate) - 1 <= detail::TokenMatcher::kMaxToken, "kDuplicate exceeds kMaxToken");
static_assert(sizeof(kWrittenLabel) - 1 <= detail::TokenMatcher::kMaxToken,
              "kWrittenLabel exceeds kMaxToken");

// A count digit run is read only to decide >= 1; cap the accumulator so a pathological run never overflows.
constexpr unsigned kCountCap = 1000000u;

}  // namespace

void detail::TokenMatcher::init(const char* t) {
    token = t;
    len = std::strlen(t);
    // KMP failure function: failure[i] is the length of the longest proper prefix of token[0..i] that is
    // also a suffix of it. It is what makes a mismatch fall back to a shorter still-valid partial match
    // instead of discarding everything (the bug a naive first-char restart has on a self-overlapping token).
    failure[0] = 0;
    size_t k = 0;
    for (size_t i = 1; i < len; ++i) {
        while (k > 0 && token[i] != token[k]) k = failure[k - 1];
        if (token[i] == token[k]) ++k;
        failure[i] = k;
    }
}

bool detail::TokenMatcher::feed(char c) {
    while (matched > 0 && c != token[matched]) matched = failure[matched - 1];
    if (c == token[matched]) ++matched;
    if (matched == len) {
        matched = failure[len - 1];  // allow an overlapping next match to begin seamlessly.
        return true;
    }
    return false;
}

WpaSecResponseClassifier::WpaSecResponseClassifier() {
    dupMatcher_.init(kDuplicate);
    writtenMatcher_.init(kWrittenLabel);
}

void WpaSecResponseClassifier::feedChar(char c) {
    // Duplicate can appear anywhere and outranks everything; match it on every byte, independent of the
    // written-count state machine (mirrors the whole-string strstr over the entire body).
    if (dupMatcher_.feed(c)) {
        duplicate_ = true;
        return;  // terminal; nothing else can change the verdict.
    }

    // Written-count state machine. A char that ends one phase is re-offered to the next (reprocess), so a
    // char terminating a count run can still begin a fresh label match in the same step. Bounded: at most
    // one re-offer per byte (ReadingCount -> Scanning), so no unbounded loop.
    bool reprocess = true;
    while (reprocess) {
        reprocess = false;
        switch (writtenState_) {
            case WrittenState::Scanning:
                if (writtenMatcher_.feed(c)) writtenState_ = WrittenState::AfterLabel;
                break;
            case WrittenState::AfterLabel:
                // Scan the rest of the line to the first digit (countOnLine semantics): a digit opens the
                // count; a newline ends the line with no count (this occurrence contributes nothing); any
                // other char is skipped.
                if (c >= '0' && c <= '9') {
                    countValue_ = static_cast<unsigned>(c - '0');
                    writtenState_ = WrittenState::ReadingCount;
                } else if (c == '\n') {
                    writtenState_ = WrittenState::Scanning;
                }
                break;
            case WrittenState::ReadingCount:
                if (c >= '0' && c <= '9') {
                    if (countValue_ < kCountCap) {
                        countValue_ = countValue_ * 10u + static_cast<unsigned>(c - '0');
                    }
                } else {
                    // The digit run ended: a count >= 1 is a success (latched, never terminal — a later
                    // "already in database" still outranks it). Re-offer this terminating char to Scanning.
                    if (countValue_ >= 1u) accepted_ = true;
                    writtenState_ = WrittenState::Scanning;
                    reprocess = true;
                }
                break;
        }
    }
}

void WpaSecResponseClassifier::feed(const char* data, size_t len) {
    if (data == nullptr) return;
    for (size_t i = 0; i < len && !duplicate_; ++i) feedChar(data[i]);
}

UploadResult WpaSecResponseClassifier::result() const {
    if (duplicate_) return UploadResult::Duplicate;
    if (accepted_) return UploadResult::Accepted;
    // No duplicate and no positive written count: "No valid handshakes/PMKIDs found", all-zero summaries, a
    // truncated read, or an unrecognised body. Fail closed — keep the capture and retry (quality bar §3).
    return UploadResult::Rejected;
}

UploadResult classifyWpaSecResponse(const char* response) {
    if (response == nullptr) return UploadResult::Rejected;
    WpaSecResponseClassifier classifier;
    classifier.feed(response, std::strlen(response));
    return classifier.result();
}

}  // namespace sapper
