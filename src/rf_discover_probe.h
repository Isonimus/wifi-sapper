/**
 * @file rf_discover_probe.h
 * @brief Bench scaffolding for the slice-0014 on-air verify: sweep the band and report the APs the
 *        pure registry discovers over serial (ADR-0013 decision #7). Device-only.
 *
 * There is no shipped sweep loop yet — the endless AutoHunt loop and the promiscuous-vs-STA radio
 * switch belong to the HuntEngine (slice-4). This probe exists only so `verify:rf-discover` has real
 * firmware to drive: gated behind `SAPPER_TEST_HOOKS`, active only when `SAPPER_TEST_RF_HOP` (a
 * channel list) is set, and compiled to no-op stubs in every shipped build so no released binary hops.
 */
#pragma once

namespace sapper {

/// Start the hop-and-discover sweep if configured. Returns true when the probe took over the device
/// (a channel list was set and promiscuous mode began), meaning main.cpp should skip the normal boot;
/// false when inactive (no channel list configured) or in a build without test hooks.
bool rfDiscoverProbeBegin();

/// Whether the probe is running and owns loop().
bool rfDiscoverProbeActive();

/// Pump once per loop(): advance the hop schedule (retuning on a hop) and report each newly
/// discovered AP, plus a liveness heartbeat. No-op unless the probe is active.
void rfDiscoverProbePump();

}  // namespace sapper
