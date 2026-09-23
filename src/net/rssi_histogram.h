/**
 * @file rssi_histogram.h
 * @brief Pure RSSI↔capture accumulator for the untethered RSSI probe (ADR-0067). Host-tested.
 *
 * The multi-hour probe runs detached from any laptop, so it cannot stream capture attempts over serial;
 * it accumulates them into this compact histogram in RAM, persists it to flash, and dumps it when the
 * board is reattached (rssi_probe.cpp). This header holds the *pure* policy — which RSSI band a reading
 * falls in, how an attempt is recorded, when a new run resets the counters, and the fixed byte layout it
 * persists as — with no filesystem or radio dependency, so the whole thing is unit-tested on the native
 * lane. A bug here silently corrupts a run nobody is watching, which is exactly why it is not device glue.
 *
 * The counters are identity-free (per-band counts only, no BSSID/SSID), so the persisted file and the
 * serial dump satisfy §4 #25 by construction.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace sapper {

/// RSSI bands in dBm, strongest first. Edges are lower-inclusive and open at the extremes so *every*
/// int8 reading maps to exactly one band (the harness bug ADR-0067's review caught was an uncovered
/// `rssi >= 0` range falling into the weakest bin — here band 0 is open at the top, so a 0-or-positive
/// reading is "very strong", never "very weak"). Kept in sync with scripts/0068-rssi-probe.mjs labels.
constexpr size_t kRssiBandCount = 5;

/// A capture attempt whose BSSID had no beacon RSSI recorded is counted here, out of the bands — the
/// probe's kRssiUnknown sentinel is 0 dBm (an impossible RX level), so a genuine 0 reading is treated as
/// unknown too (documented collision, negligible in practice).
constexpr int8_t kRssiUnknown = 0;

/// Max run-tag length (including the terminator). A run tag identifies one field run; a boot whose build
/// carries a different tag than the persisted file starts the counters fresh (see resetForTag). Sized to
/// hold a realistic date+location+session tag; a tag that would not fit is rejected loud (runTagFits),
/// never truncated — two tags that differ only past the limit must not silently collide into one run.
constexpr size_t kRssiRunTagLen = 48;

/// Identity-free per-band capture counts plus an unknown-RSSI bucket, tagged by run. `attempts[b]` is how
/// many capture windows opened on an AP in band b; `captured[b]` how many yielded a valid handshake. The
/// capture rate per band is the correlation ADR-0067 needs.
struct RssiHistogram {
    char runTag[kRssiRunTagLen] = {0};
    uint32_t attempts[kRssiBandCount] = {0};
    uint32_t captured[kRssiBandCount] = {0};
    uint32_t naAttempts = 0;  ///< attempts whose BSSID had no recorded beacon RSSI.
    uint32_t naCaptured = 0;
};

/// The band index [0, kRssiBandCount) for @p rssi — total over every int8 value (band 0 open at the top,
/// band kRssiBandCount-1 open at the bottom). Not called for kRssiUnknown; record() routes that itself.
size_t rssiBand(int8_t rssi);

/// Record one resolved capture attempt: @p captured is whether a valid handshake was collected. An
/// @p rssi of kRssiUnknown lands in the na bucket; any other value in its band.
void recordAttempt(RssiHistogram& hist, int8_t rssi, bool captured);

/// Adopt @p tag, zeroing every counter — the start of a fresh run. Truncates a tag at kRssiRunTagLen-1
/// as a last-resort defence; callers gate on runTagFits() first so truncation never actually happens.
void resetForTag(RssiHistogram& hist, const char* tag);

/// Whether @p tag fits the run-tag field with its terminator (strlen < kRssiRunTagLen). The probe rejects
/// a tag that does not fit rather than storing a truncated form — otherwise two distinct runs whose tags
/// differ only past the limit would collide into one (a silent data merge).
bool runTagFits(const char* tag);

/// Whether @p hist was written for @p tag — an exact match, the resume-vs-fresh-run decision. Only valid
/// once @p tag has passed runTagFits(); with that guarantee the stored and given tags are compared whole.
bool histogramHasTag(const RssiHistogram& hist, const char* tag);

/// The exact number of bytes serialize() writes / deserialize() reads (magic + version + tag + counters).
constexpr size_t kRssiHistogramBytes = 4 + 1 + kRssiRunTagLen + (kRssiBandCount * 2 + 2) * 4;

/// Serialize @p hist into @p out (must be >= kRssiHistogramBytes). Returns the bytes written, or 0 if the
/// buffer is too small (fail loud — the caller must not persist a truncated record).
size_t serializeHistogram(const RssiHistogram& hist, uint8_t* out, size_t capacity);

/// Parse a serialized histogram from @p in into @p hist. Returns false (leaving @p hist untouched) on a
/// bad magic, version, or length — the caller then starts fresh rather than trust an unknown format.
bool deserializeHistogram(const uint8_t* in, size_t len, RssiHistogram& hist);

}  // namespace sapper
