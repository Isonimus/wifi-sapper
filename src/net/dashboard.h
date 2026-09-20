/**
 * @file dashboard.h
 * @brief Pure Maintenance-dashboard rendering + the auto-resume backstop (ADR-0039). Host-tested.
 *
 * The hardware-free half of slice-0040 — no `WebServer`, `WiFi`, or `Preferences`. Sibling to
 * net/provisioning_form (ADR-0037): the two decisions that make the Maintenance dashboard correct
 * are proven off-device — what it renders (`buildDashboardHead` + `appendCrackedRow` + `kDashboardFoot`)
 * and when a walked-away unit auto-resumes hunting (`maintenanceBackstopDue`).
 *
 * WHY streaming instead of one fixed buffer (revises ADR-0039 decision 4): the page size is
 * data-dependent — the manifest holds up to kMaxCrackedEntries (256) recovered results, ~36 KB of HTML.
 * A single fixed buffer would either need a ~36 KB stack buffer this no-PSRAM board cannot spare, or
 * "fail loud if it all does not fit", which breaks the viewer on a *large* account — fail-loud on
 * success, not on a bug. So the caller streams: head once, one row per result via sendContent(), then
 * the footer. Each piece renders into its OWN small bounded buffer, where an overflow genuinely is a
 * bug (a single row cannot exceed its known max), so the fail-loud-returns-0 guarantee stays meaningful
 * (§3) while the whole page is never materialised at once.
 *
 * The dashboard deliberately serves recovered plaintext PSKs (ADR-0039 decision 5) — a local viewer on
 * the operator's own hardened SoftAP, the opposite threat model to the off-device webhook (§4 #14/#19).
 * That is why there is no secret-free view-model here (unlike SetupFormModel, §4 #20): appendCrackedRow
 * is handed the CrackedResult, PSK and all, on purpose.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "net/cracked_result.h"  // CrackedResult — the recovered {essid, psk} the dashboard renders.

namespace sapper {

/// The persisted, engine-free facts the dashboard summarises (ADR-0039 decision 3). All pulled through
/// existing seams — the CrackedManifest count (#12), loadProvisioning (#6), the capture store depth — so
/// no new store-access site is created. No live session counters: entering Maintenance reboots, wiping
/// RAM, so only persisted state survives (ADR-0039 decision 1).
struct DashboardStats {
    size_t recoveredCount = 0;      ///< Recovered networks the manifest holds (its size()).
    size_t captureQueueDepth = 0;   ///< Handshakes queued for upload, awaiting the next Station window.
    bool deauthArmed = false;       ///< Whether autonomous deauth is armed (ADR-0029).
    /// Whether the device has ever completed a sync — the persisted `!CrackedManifest::isFresh()` flag.
    /// Deliberately NOT a "last sync ok / +N new": that status is a millis()-based RAM value the sync
    /// scheduler holds (cracked_store.h), wiped by the reboot that enters Maintenance, so it could only
    /// ever read back as a dishonest zero here. The dashboard shows persisted state only (ADR-0039 #1).
    bool everSynced = false;
};

/// Buffer size the caller gives buildDashboardHead(). The head is fixed-shape (~1 KB); 2 KB leaves
/// margin so an overflow here is a real bug, not a data-size effect (the rows stream separately).
constexpr size_t kDashboardHeadBufSize = 2048;

/// Buffer size the caller gives appendCrackedRow(). One row is an ESSID (<=32) + PSK (<=64), each
/// HTML-escaped (up to 6x per char — "&quot;" is the longest entity) plus markup: worst case ~700
/// bytes; 1 KB leaves margin.
constexpr size_t kDashboardRowBufSize = 1024;

/**
 * @brief Render the dashboard head — page open, the summary block, and the results table opening —
 *        from @p stats into @p out. Pure.
 * @return bytes written (excluding the NUL), or 0 if it does not fit @p outSize, in which case @p out
 *         is left empty and the caller must serve an error, never a truncated page (§3, fail loud).
 */
size_t buildDashboardHead(const DashboardStats& stats, char* out, size_t outSize);

/**
 * @brief Append one recovered network as a table row — its ESSID and its plaintext PSK — into @p out.
 *        Pure. The ESSID and PSK are HTML-escaped (they arrive from the air / an external service), so a
 *        crafted ESSID cannot inject markup into the served page.
 * @return bytes written (excluding the NUL), or 0 if the row does not fit @p outSize (a bug at this size,
 *         so fail loud rather than emit a half-row).
 */
size_t appendCrackedRow(const CrackedResult& result, char* out, size_t outSize);

/// The dashboard footer: closes the table and the document. A fixed string (no formatting, so no
/// overflow risk), streamed last after the head and every row.
extern const char kDashboardFoot[];

// The no-activity auto-resume backstop (ADR-0039 decision 7): a walked-away Maintenance unit reboots
// into Station so it cannot self-DoS its endless-hunt identity, while an in-use dashboard (which pushes
// the activity time forward on each request) never self-reboots mid-review.
constexpr uint32_t kMaintenanceBackstopMs = 30u * 60u * 1000u;  // 30 minutes.

/**
 * @brief Whether the no-activity backstop is due: kMaintenanceBackstopMs has elapsed since the last
 *        recorded activity (ADR-0039 decision 7). Pure.
 *
 * Unsigned difference, so it is correct across the millis() 32-bit wrap (the 30-min interval is far
 * below the ~49-day wrap period). Recording activity (advancing @p lastActivityMs toward @p nowMs)
 * pushes the due time forward, so serving a request keeps the dashboard alive.
 */
bool maintenanceBackstopDue(uint32_t lastActivityMs, uint32_t nowMs);

}  // namespace sapper
