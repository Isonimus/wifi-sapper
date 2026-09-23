---
id: '0067'
title: "Defer smart target ordering to a measured RSSI lever"
type: architecture
status: accepted
date: 2026-09-22
supersedes: []
superseded_by: []
---

## Context

A LEDGER item proposed **smart target ordering** — ordering the discovered-AP round-robin by
`RSSI + WPA2-preference + channel-grouping`, borrowed from the parent Adversary's autohunt scorer
(`RSSI + 100`, `+10` for WPA2-PSK, attack strongest-first). Before building it, each of the three
components was weighed against how *this* firmware actually captures, and two of the three do not earn
their place.

- **Channel grouping ("capture every AP on one channel before retuning") does not cut measurable
  thrash.** The hunt captures **one target at a time for a full `captureWindowMs` (8000 ms)**, and
  retunes exactly once per target at the start of Capturing (`HuntEngine::doStartCapturing` →
  `retuneTo`). The retune rate is therefore **one `esp_wifi_set_channel` per 8 s regardless of target
  order**. Grouping by channel could at most drop the redundant same-channel retunes (and only with an
  added "skip if already parked" guard), saving on the order of a `setChannel` call — single-digit
  milliseconds — spread across a capture cycle of ~`targets × 8 s`: a fraction of a percent. The
  "cuts retune thrash" rationale is real in *count* but negligible in *effect*, because the 8 s window
  amortises the per-target retune to nothing. Grouping would matter only under a *different* capture
  model — simultaneous multi-AP capture per channel (one channel parked, all its APs' handshakes
  collected at once) — which is a far larger redesign than "sort the snapshot" and is not what the item
  proposed.

- **WPA2-preference is subsumed by skip-open (ADR-0063 #29).** Open networks are already skipped
  entirely, so the only thing left to "prefer WPA2 over" is WEP — and WEP-vs-WPA discrimination needs
  the RSN/vendor-IE parsing ADR-0063 decision 1 deliberately rejected for an extinct case. WPA2
  preference thus reduces to ordering an extinct case first: marginal at best.

- **RSSI-first is the one real lever, and it is not free.** Attacking strongest-first is the genuine
  prize: a stronger AP has better SNR, is likelier to yield M1–M4, and responds to deauth more
  reliably, so in an endless hunt with bounded capture windows it should raise yield-per-hour. But the
  Adversary gets RSSI free from `WiFi.scanNetworks()`, whereas our promiscuous-sniffer discovery would
  need the `FrameConsumer`/`RadioSniffer` seam widened to carry `wifi_pkt_rx_ctrl_t.rssi` — the
  boundary §4 #8 has deliberately kept to raw frame bytes since slice-0014. That is a real
  architectural cost, and §3/§4 forbid paying it on an estimate.

The measure-first discipline has a catch worth recording: the measurement is **not** a cheap bench
probe. "Does RSSI-order beat discovery-order on capture yield?" is a multi-hour **field** experiment
needing real APs with real clients over time. A probe can sidestep the *seam* (read `rx_ctrl.rssi`
directly in a throwaway build — probes are exempt from the seam, §3) but not the wall-clock time on
hardware.

## Decision

1. **Do not build channel grouping.** Under the one-target-per-8 s capture model its measurable benefit
   is a fraction of a percent (above). It is recorded here as a rejected non-feature so a future reader
   does not re-add it on the same "cuts retune thrash" premise. It would become worth revisiting only if
   the capture model changes to simultaneous per-channel multi-AP capture — a separate decision with its
   own ADR.

2. **Do not build WPA2-preference.** Skip-open (#29) already removes the only networks it would
   deprioritise except WEP, and WEP ordering is not worth the IE walk #29 rejected.

3. **Gate RSSI-first ordering on a field-measured yield gain.** No production ordering code and no §4 #8
   seam widening are written until an on-hardware experiment shows RSSI-order capturing measurably more
   handshakes per unit time than discovery-order. That experiment is a throwaway RSSI probe build
   (reads `rx_ctrl.rssi` without touching the production seam) plus a field run; its numbers land in a
   follow-up ADR, which is where the seam decision is then made.

4. **No standing invariant, no slice.** This decision records a deferral and two rejections; it adds no
   definition-of-done rule future work must satisfy, so it adds no §4 row, and it ships no feature
   work-unit, so it has no slice.

## Consequences

- The `[feature] capture: smart target ordering` LEDGER item is replaced by a single narrower deferral:
  run the RSSI yield field experiment; build the seam + ordering only if it wins. The channel-grouping
  and WPA2-preference halves are closed here, not carried forward.
- The §4 #8 raw-bytes seam boundary is **unchanged** — this ADR is the record of a decision *not* to
  widen it yet, which strengthens rather than weakens it.
- The endless hunt keeps targeting in discovery order (first-sighting-wins, `ApRegistry` append order),
  filtered by skip-open (#29) and the optional cracked/submitted gates (#27/#28/#30). Ordering within
  that set is unchanged.
- If the field experiment is never run, the cost is only the opportunity cost of un-optimised ordering —
  never a missed handshake. Discovery-order captures every targetable AP; it just may not capture the
  easiest ones first.

## Amendment — 2026-09-22: measure the necessary condition (RSSI↔capture correlation), not a full A/B

Decision 3 framed the experiment as an A/B of "RSSI-order vs discovery-order handshakes per unit time".
The first probe built (slice-less, `SAPPER_TEST_RSSI`, `scripts/0068-rssi-probe.mjs`) instead measures
the **necessary condition** for that A/B to ever pay off: does a stronger beacon RSSI predict a higher
capture rate *at all*, in our environment? It runs the real unmodified `HuntEngine` in discovery order,
taps `rx_ctrl.rssi` per beacon out-of-band in a throwaway probe sniffer (production seam untouched), and
emits one identity-free row per capture attempt — `rssi_dbm, channel, ms_in_window, outcome` — with no
BSSID, so §4 #25 needs no redaction here. Offline the rows bin by RSSI and compare capture rate.

Why this is the right first measurement, not a deviation from the intent: an A/B is worth its two long,
client-drift-confounded runs and its throwaway ordering prototype only if RSSI carries signal about
capture success. If the correlation is null, RSSI-ordering cannot raise yield and the seam is not built —
established in one run, un-confounded (strong and weak APs are measured together). If the correlation is
strong, that is already sufficient evidence to widen the seam; the A/B becomes optional confirmation, not
the gate. Either way the number lands in the follow-up ADR that makes the seam decision (decision 3
unchanged in substance: measure on hardware before widening §4 #8).

## Amendment — 2026-09-23: untethered run → persisted histogram, and gates deliberately off

Two field realities reshaped the probe. First, the lab's handshakes are all already uploaded to wpa-sec,
so a gated hunt would skip every AP and record nothing. This is a non-issue by construction: the probe
builds a bare `HuntEngine` with **no** `setCrackedLookup`/`setSubmittedLookup` wired and never uploads, so
it targets and attempts *every* encrypted AP regardless of wpa-sec state, and "captured" is a pure on-air
capture event (fresh ANonce/SNonce every time — re-capturing an already-uploaded AP is exactly as easy as
the first). Gates-off is not just convenient, it is *required*: the correlation needs the full AP
population across the RSSI range, which gating would bias to the uncracked subset. **No cracked/submitted
memory wipe is needed to run the probe.**

Second, the multi-hour run is **untethered** — no laptop attached, so no serial is visible during it. The
per-attempt serial row of the first amendment cannot be the record. So the probe now bins each resolved
attempt on-device into a compact per-band histogram (`net/rssi_histogram.{h,cpp}`, a pure, host-tested
unit — because a bug in an unwatched accumulator silently corrupts the whole run) and persists it to
LittleFS with the temp-then-rename idiom the submitted/cracked stores use (power-loss safe across the
run). On reattach the firmware dumps the identity-free histogram to serial at boot; `probe:rssi` reads
that block and writes the correlation table. A run tag (`SAPPER_TEST_RSSI_RUN`, stored in the file)
resolves resume-vs-reset: a matching tag resumes across a field power-cycle, a differing tag starts fresh
— RTC/reboot markers cannot do this, since a power-loss clears RTC and would wipe the very data the run is
trying to survive. The per-attempt row is kept only for the tethered smoke test. The histogram is per-band
counts only (no BSSID), so §4 #25 holds by construction, on flash and on the wire.
