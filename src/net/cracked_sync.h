/**
 * @file cracked_sync.h
 * @brief The pure sync orchestration: fetch → parse → mirror → alert (ADR-0019 decisions #3, #5, #7).
 *
 * Ties the download seam (cracked_fetcher.h), the pure parser (cracked_result_parser.h), and the
 * per-BSSID manifest (cracked_manifest.h) into one operation, runSync(), and emits the appliance's
 * only outputs for this half of the loop onto the surface event bus (core/event_bus.h) — new-password,
 * first-sync summary, and sync-outcome events (ADR-0019 decision #7; ADR-0021 moved the transport from a
 * bespoke observer to the shared bus; slice-7's surfaces subscribe). It owns no hardware and no
 * filesystem, so the whole policy is host-tested on the native lane (ADR-0004 lane 1; slice-0020
 * Scenarios E, F, G, I).
 *
 * The order guarantees ADR-0019 decision #7 and Scenario I: the parsed results are buffered as the
 * fetch streams, and the manifest is touched only after the fetch returns Ok with a usable body — a
 * transport failure or an all-malformed (garbage/truncated) body leaves the manifest exactly as it was
 * and raises no new-password or summary event. Persistence precedes announcement: entries are flushed
 * to the store before any new-password event fires, so the appliance never announces a crack it did
 * not durably record. The first sync of a fresh manifest seeds silently and emits one summary rather
 * than one alert per backlog entry (decision #5).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/event_bus.h"
#include "net/cracked_fetcher.h"
#include "net/cracked_manifest.h"
#include "net/cracked_result.h"
#include "net/cracked_result_parser.h"
#include "net/pwncrack_potfile_parser.h"

namespace sapper {

/// The counted result of one sync — the sync-outcome event's payload and how a verify/surface reports
/// a cycle. ok is the single "did this sync succeed" gate the scheduler notes; it is true only when
/// *every provisioned* source fully transferred a usable body and the apply persisted (ADR-0109 #5).
struct SyncOutcome {
    FetchResult fetch = FetchResult::Transport;  ///< How the wpa-sec download transfer itself went.
    FetchResult pwncrackFetch = FetchResult::Ok; ///< How the pwncrack download went (meaningful iff pwncrackRan).
    bool pwncrackRan = false; ///< pwncrack was provisioned and fetched this sync (ADR-0109 #5).
    bool ok = false;          ///< The whole sync succeeded (every provisioned source usable + persisted).
    bool firstSync = false;   ///< This sync seeded a fresh (never-yet-synced) manifest.
    size_t downloaded = 0;    ///< Results parsed Ok across both sources' bodies.
    size_t malformed = 0;     ///< Lines skipped as malformed (a non-zero count is a format-change signal).
    size_t newPasswords = 0;  ///< New/changed entries announced (0 on a first/seed sync).
    size_t overflow = 0;      ///< Results beyond kMaxCrackedEntries the buffer could not hold (LEDGER).
    bool storeError = false;  ///< The post-apply flush failed — applied in RAM but not persisted (§3).
};

/**
 * @brief Runs one wpa-sec cracked-results sync over injected seams.
 *
 * Construct with the fetcher, the manifest, the wpa-sec key, and the surface event bus; call
 * runSync(now) when the scheduler marks a sync due and an STA window is up (the UploadSupervisor
 * drives that — ADR-0019 decision #6). Holds a fixed result buffer for the atomic fetch-then-apply;
 * no heap.
 *
 * It publishes the three sync facts ADR-0019 decision #7 defined — SyncCompleted, NewPassword, and
 * FirstSyncSummary — onto the bus (ADR-0021 refined the transport from a bespoke SyncEventObserver to
 * the shared bus; the facts and their timing are unchanged). slice-6 emits; slice-7's surfaces
 * subscribe.
 */
class CrackedSync : private CrackedLineSink {
public:
    CrackedSync(CrackedResultsFetcher& fetcher, CrackedManifest& manifest, const char* wpaSecKey,
                EventBus& bus)
        : fetcher_(fetcher), manifest_(manifest), wpaSecKey_(wpaSecKey), bus_(bus) {}

    /// Wire pwncrack as an optional second download source feeding the SAME manifest (ADR-0109 #5). Off
    /// by default: with no source set, or an empty key, runSync behaves exactly as the wpa-sec-only sync
    /// (a wpa-sec-only device is byte-for-byte unchanged, like the optional webhook/upload backend). The
    /// pwncrack source uses the pwncrack potfile parser; the manifest, fresh/seed policy, and the one
    /// NewPassword alert stay backend-agnostic (#5).
    void setPwncrackSource(CrackedResultsFetcher& fetcher, const char* key) {
        pwncrackFetcher_ = &fetcher;
        pwncrackKey_ = key;
    }

    /// True when a pwncrack source is wired with a non-empty key, so runSync runs it this cycle.
    bool pwncrackProvisioned() const {
        return pwncrackFetcher_ != nullptr && pwncrackKey_ != nullptr && pwncrackKey_[0] != '\0';
    }

    /// Fetch, parse, mirror, and announce, returning the counted outcome. Fetches every provisioned
    /// source (wpa-sec, then pwncrack if wired) into one buffer, then applies it to the manifest once.
    /// A source whose transfer failed or yielded only malformed lines contributes nothing and is rolled
    /// back (per-source atomicity, Scenario I), but never blocks a sibling source's good results from
    /// applying; the manifest is persisted only when a result was applied or the first-sync baseline
    /// completed, and ok is true only when every provisioned source was usable (ADR-0109 #5).
    SyncOutcome runSync(uint32_t nowMs);

private:
    /// Which source's lines onLine is currently parsing: selects the (sibling) parser per source (#5).
    enum class Source { WpaSec, Pwncrack };

    /// CrackedLineSink: parse one streamed line into the result buffer, counting malformed/overflow.
    void onLine(std::string_view line) override;

    /// Fetch one source into the buffer, with per-source atomic rollback. Returns the fetch result and
    /// clears @p allUsable if this source did not fully transfer a usable body (so a truncated/garbage
    /// source neither mutates the manifest nor completes the first-sync baseline).
    FetchResult fetchSource(CrackedResultsFetcher& fetcher, const char* key, Source src, bool& allUsable);

    CrackedResultsFetcher& fetcher_;
    CrackedManifest& manifest_;
    const char* wpaSecKey_;
    EventBus& bus_;

    CrackedResultsFetcher* pwncrackFetcher_ = nullptr;  ///< Optional second source (ADR-0109 #5); unset = off.
    const char* pwncrackKey_ = nullptr;                 ///< pwncrack key; empty/null disables the source.
    Source activeSource_ = Source::WpaSec;              ///< The source onLine is parsing right now.

    // Fetch scratch, reset at the start of each runSync(). The parsed account is buffered here so the
    // manifest is touched only after the fetch returns Ok (Scenario I); alertIdx_ records which buffered
    // results were New/Changed so they are announced only after a successful flush.
    CrackedResult buffer_[kMaxCrackedEntries];
    uint16_t alertIdx_[kMaxCrackedEntries];
    size_t bufferCount_ = 0;
    size_t malformed_ = 0;
    size_t overflow_ = 0;
};

}  // namespace sapper
