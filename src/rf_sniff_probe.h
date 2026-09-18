/**
 * @file rf_sniff_probe.h
 * @brief Bench scaffolding for the slice-0012 on-air verify: run the fixed-channel sniffer and
 *        report what reaches the pure core over serial (ADR-0011 decision #5). Device-only.
 *
 * There is no shipped capture loop yet — the endless AutoHunt loop and the promiscuous-vs-STA radio
 * switch belong to the HuntEngine (slice-4). This probe exists only so `verify:rf-sniffer` has real
 * firmware to drive: gated behind `SAPPER_TEST_HOOKS`, active only when `SAPPER_TEST_RF_BSSID` is
 * set, and compiled to no-op stubs in every shipped build so no released binary sniffs.
 */
#pragma once

namespace sapper {

/// Start the sniffer if configured. Returns true when the probe took over the device (a target
/// BSSID was set and promiscuous mode began), meaning main.cpp should skip the normal boot; false
/// when inactive (no target configured) or in a build without test hooks, so the normal boot runs.
bool rfSniffProbeBegin();

/// Whether the probe is running and owns loop().
bool rfSniffProbeActive();

/// Pump once per loop(): report the target's beacon when it first reaches the core, plus a liveness
/// heartbeat. No-op unless the probe is active.
void rfSniffProbePump();

}  // namespace sapper
