/**
 * @file wifi_station.h
 * @brief Station association and NTP clock sync (ADR-0006, slice-0007 phases 2-3). Device-only.
 *
 * Adapted from the Adversary's `WiFiConnection` and `TimeManager` — not linked: the parent's are
 * entangled with its UI and SD config, which the Sapper deliberately does not reuse (slice-0007
 * Design). Device-only (`WiFi`, `configTime`); proven on hardware by the verify script (Scenario
 * D). The pure boot gate that decides whether we reach these at all is host-tested (lane 1).
 */
#pragma once

#ifndef UNIT_TEST

namespace sapper {

/// Called once per poll while a bounded boot wait blocks, so the serial channel keeps draining
/// (CLAUDE.md §4 invariant #3 — every pre-engine blocking state pumps serial). A plain function
/// pointer, not std::function: zero heap, and the one caller passes a free function.
using BootTick = void (*)();

/**
 * @brief Associate with @p ssid using @p pass (empty for an open network), bounded by a timeout.
 * @param tick pumped each poll so `state`/`ping` stay answered during the wait (may be nullptr).
 * @return true once the station has an IP; false if association did not complete in time.
 *
 * One attempt. The boot gate (decideBootPhase) owns the retry budget across attempts, so this
 * stays a single bounded try and reports failure loudly rather than looping forever.
 */
bool connectStation(const char* ssid, const char* pass, BootTick tick);

/**
 * @brief Sync the system clock over NTP, bounded by a timeout.
 * @param tick pumped each poll so the serial channel stays responsive during the wait.
 * @return true once the clock reads past the year-2020 sentinel; false on timeout.
 *
 * The Sapper's wpa-sec uploads are TLS, and certificate validity cannot be checked with a clock
 * still at the 1970 epoch — so `Ready` is gated on a real time, not merely on association. UTC;
 * local offset is irrelevant to a headless appliance and to TLS validity.
 */
bool syncClock(BootTick tick);

}  // namespace sapper

#endif  // UNIT_TEST
