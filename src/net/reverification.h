/**
 * @file reverification.h
 * @brief The bounded rotated-PSK re-verification policy: constants + the staleness test (ADR-0072).
 *
 * The rotated-PSK gates (A/B/C — ADR-0059/-0061/-0065, §4 #27/#28/#30) skip a known network by
 * *membership*, which is permanent until LRU eviction, so a rotated PSK is never re-captured and never
 * re-cracked. ADR-0072 adds a cadence: an entry whose persisted wall-clock `lastVerifiedEpoch` is older
 * than kReverifyIntervalSec is treated as not-a-member for one re-verification pass (re-targeted and
 * re-uploaded), then re-suppressed when the fresh handshake lands. This header is the one home for the
 * policy — the interval, the valid-epoch guard, and the pure staleness test both manifests consult — so
 * the subtle part lives (and is tested) in a single place rather than duplicated across the two.
 */
#pragma once

#include <cstdint>

namespace sapper {

/// How long a verified network stays suppressed before it is re-targeted/re-uploaded once more (ADR-0072
/// decision 6). 7 days: long enough that the re-work is one extra capture+upload per known network per
/// week (negligible against the endless hunt), short enough to catch a key rotation within a week. An
/// architecture-derived default, not a measurement — tuning against observed rotation periods is deferred
/// (LEDGER), like dwell/settle/deauth-cadence/backoff. A constexpr, not a runtime field (KISS): a tuning
/// pass supersedes the value, it is never per-instance.
constexpr uint32_t kReverifyIntervalSec = 7u * 24u * 60u * 60u;  // 604800

/// The threshold distinguishing a real NTP-synced clock from the 1970 power-on default (2020-01-01 UTC).
/// Below it the cadence is inert — no entry is ever stale — so the pre-cadence gate behaviour holds
/// verbatim before the first NTP sync and on a device that never associates (ADR-0072 decision 3). This
/// deliberately duplicates wifi_station.cpp's device-only TLS `kMinValidEpoch`; left as two instances
/// (rule of three — second; ADR-0072 consequences), not hoisted.
constexpr uint32_t kMinRealEpoch = 1577836800u;

/// Whether an entry last verified at @p lastVerifiedEpoch is due for re-verification against the current
/// @p nowEpoch (ADR-0072 decision 2). Returns false — never due, so the gate keeps skipping the member —
/// while @p nowEpoch is below kMinRealEpoch (no real clock yet). The age is computed backwards-clock-safe
/// (a @p lastVerifiedEpoch ahead of @p nowEpoch yields age 0, never a huge unsigned wrap), so a clock
/// adjustment cannot spuriously force a re-verification.
inline bool reverifyDue(uint32_t lastVerifiedEpoch, uint32_t nowEpoch) {
    if (nowEpoch < kMinRealEpoch) return false;
    const uint32_t age = nowEpoch >= lastVerifiedEpoch ? nowEpoch - lastVerifiedEpoch : 0u;
    return age >= kReverifyIntervalSec;
}

}  // namespace sapper
