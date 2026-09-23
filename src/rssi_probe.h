/**
 * @file rssi_probe.h
 * @brief On-device RSSI↔capture-correlation probe (ADR-0067). Device-only, SAPPER_TEST_HOOKS-gated.
 *
 * The measurement ADR-0067 defers to hardware: does a stronger beacon RSSI predict a higher capture
 * rate? It runs the real, unmodified HuntEngine in normal discovery order and taps `rx_ctrl.rssi`
 * out-of-band in a throwaway probe sniffer (the production RadioSniffer seam is never touched, so §4 #8
 * holds). For each capture attempt the engine makes it emits one identity-free row
 * `[RSSI] rssi=<dbm> channel=<n> ms=<window> outcome=<captured|empty>` — no BSSID reaches serial, so §4
 * #25 needs no redaction. `scripts/0068-rssi-probe.mjs` collects the rows and bins them by RSSI.
 *
 * A probe, not a wired verify (docs/quality-bar §3): it answers its question once and the number lands
 * in the follow-up seam-decision ADR. Inactive unless SAPPER_TEST_RSSI names a channel list, so this
 * hooks build still boots normally otherwise, like every other probe (main.cpp dispatch).
 */
#pragma once

namespace sapper {

/// Bring up the probe if SAPPER_TEST_RSSI is set (a channel list). Returns false when inactive (normal
/// boot proceeds) or on a fatal bring-up error (logged [FATAL]).
bool rssiProbeBegin();

/// True once rssiProbeBegin() took over the device — the normal boot loop is then skipped.
bool rssiProbeActive();

/// Drive one iteration: tick the engine, emit any resolved capture-attempt rows, heartbeat.
void rssiProbePump();

}  // namespace sapper
