---
id: '0019'
title: "wpa-sec cracked-results sync — full-account download, per-BSSID manifest, and delta alerts"
type: architecture
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Context

ADR-0017 closed the capture→upload half of the appliance's core loop and, in decision #6, deliberately
left the other half to this slice: "the hourly cracked-results sync (slice-6) shares the STA session the
drain already brings up ... but that scheduling is slice-6's to decide." Uploads now leave the device;
nothing yet brings the *results* back. Without this, the Sapper is write-only — it feeds wpa-sec and
never learns which handshakes cracked, which is the one output the operator actually wants from a
headless appliance they cannot watch.

The download side of the wpa-sec API is not guessed here: it is confirmed against a working
implementation, `adversary:src/modules/network/wpasec_service.cpp` (`fetchCrackedResults`), which has
run this exact endpoint on the same hardware. Its measured contract:

- **`GET https://wpa-sec.stanev.org/?api&dl=1`**, authenticating with the same `Cookie: key=<key>` the
  upload uses — same host, same key, same pinned-TLS anchor and NTP gate as ADR-0017 decision #1.
- The response body is the account's **entire** cracked set, one result per line:
  `ap_bssid:client_bssid:essid:password`. There is **no** server-side "only new since" filter — every
  sync returns everything the account has ever cracked, so "new vs. already-known" is necessarily a
  *local* computation.

That last fact is the design's hinge, and it surfaces the operator's real question: an account may
already hold cracked results from other firmwares or manual uploads before this appliance ever runs.
Three forces shape the answer.

1. **Headless-first (ADR-0001).** The appliance has no operator watching a screen; its whole value is
   announcing recovered passwords. The results must land in a durable store that any surface (slice-7)
   can read, and the announcement must be an *event*, not a UI call.
2. **The backlog must not become alert spam.** If every sync re-downloads the whole account and every
   downloaded line were announced, the first sync of an account with an existing backlog would emit a
   burst of "new password!" alerts for passwords cracked long before the device existed — crying wolf on
   the exact channel that must stay trustworthy for the genuine future crack.
3. **The reference implementation has two defects not to port.** (a) It parses each line with
   `strtok_r(line, ":")` and keeps the 4th token as the password, so a password containing `:` (common)
   is silently truncated. (b) It filters downloaded results to those matching a capture *on that device*
   — correct for an interactive tool showing its own captures, wrong for an appliance whose job is to
   mirror the operator's whole account.

## Decision

**1. The download reuses ADR-0017's TLS anchor and NTP gate, but reads through `HTTPClient` so chunked
transfer is de-framed by the stack, not by us.** The fetch trusts the same pinned self-signed GTS Root
R4 (`wpasec_root_ca.h`) over a `WiFiClientSecure`, and is gated on a real NTP clock exactly as the
upload is — a pinned cert cannot be validated against a 1970 epoch (ADR-0017 decision #1). It differs
from the upload in one respect: the upload *response* was short and read over raw TLS, and ADR-0017
decision #2 ledgered that raw-stream parse as fail-closed-acceptable precisely because a split success
token only ever costs a retry. The download body is the opposite — **large and line-structured** (every
cracked result the account holds), so a `Transfer-Encoding: chunked` boundary splitting a line is a real
data-corruption risk, not a rare false-reject. Rather than hand-roll a de-chunker, the fetch reads the
body through `HTTPClient` over the pinned `WiFiClientSecure` (the reference's proven approach), which
de-chunks via its stream, keeping the CA pin intact. This resolves, for the read path, the concern
ADR-0017 decision #2 deferred; that ledger item stays open only for the upload *response* path it named.

**2. The Sapper mirrors the whole account, not the device's own captures.** Unlike the reference, which
applies a downloaded result only when it matches a local capture (BSSID first, SSID fallback), the
Sapper stores **every** parsed result. This is the deliberate divergence the operator chose: a headless
appliance's job is to report every password wpa-sec holds for the account — including the pre-existing
backlog and results other units captured — not only what this one unit happened to catch. There is no
capture-matching step; the manifest below simply becomes the local mirror of the account's cracked set.

**3. The line parser is pure, host-tested, and splits on the first three colons only.** A new pure unit,
`CrackedResultParser`, turns the body into `CrackedResult { uint8_t bssid[6]; char essid[...]; char
password[...]; }` records. It splits each line on the **first three** delimiters and keeps the entire
remainder as the password verbatim — so a password containing `:` survives intact, fixing the
reference's `strtok_r` truncation (force 3a). A line that has fewer than three delimiters, an
unparseable AP BSSID, or an empty essid/password is **skipped and counted**, never silently dropped and
never partially stored (§3): the sync reports a malformed-line count so a wpa-sec format change fails
loud rather than quietly parsing to nothing. Being hardware-free, it is unit-tested on the native lane
(ADR-0004 lane 1) against a real captured body — a `scripts/wpasec-download-probe.mjs` captures one live
`?api&dl=1` response (the operator holds the only key that can) and that body becomes the parser's
fixture, so the contract is pinned by measured bytes, not by this prose (the same discipline ADR-0017
decision #2 uses for the upload response).

**4. Results persist in a bounded per-BSSID `CrackedManifest` behind a seam, backed by LittleFS.** The
manifest keys by AP BSSID → `{ essid, password, firstSeenCrackedMs }` and is the single source of truth
for "what the account has cracked" — the full list surfaces (slice-7) read, and the delta detector
(decision #5) diffs against. It sits behind an abstract seam mirroring `CaptureStore` (ADR-0017 decision
#3): pure logic talks to the interface, a host test backs it with an in-RAM fake, and the device backing
is LittleFS on internal flash — the portable floor, present on every target, surviving reset. It is
bounded (`kMaxCrackedEntries`); on overflow it drops the oldest by `firstSeenCrackedMs`, the same
recency bound and rationale as the capture queue, since wpa-sec still holds the record and a re-download
re-seeds it. One BSSID may re-crack to a *different* password (rotated PSK): that is an update to the
existing entry and, per decision #5, a new alert.

**5. Alerting is edge-triggered on the manifest delta; the first sync seeds and summarises, then per-crack.**
A downloaded result raises a new-password event only when the manifest does **not** already hold that
BSSID with that same password — a genuinely new BSSID, or an existing BSSID whose password changed. A
re-download of an unchanged result is silent. The first sync of a fresh manifest is the backlog case: it
seeds every result silently and emits **one** summary event (`imported N previously-cracked results`),
not N individual alerts (force 2). "Fresh manifest" is a persisted flag, not "manifest empty", so a
genuine first real crack into a still-empty manifest is not misfiled as backlog. Every subsequent sync
emits one new-password event per genuinely-new or changed entry. The delta computation is pure and
host-tested: given a prior manifest and a parsed result set, it returns exactly the entries to announce.

**6. An hourly `SyncScheduler` decides *when*, piggybacking the drain's STA window; the fetch never opens
its own schedule of associates.** A pure, clock-driven scheduler (wrap-safe deadline, fake-clock
host-tested like every other window in the engine) marks a sync **due** once `kSyncIntervalMs` has
elapsed since the last successful sync. Per ADR-0017 decision #6 the sync shares the STA session the
drain brings up: when a drain cycle associates, a due sync runs in that same window before teardown; if
the upload queue stays empty long enough that no drain associates but the hour comes due, the scheduler
requests one STA window for the sync alone. This keeps the appliance associating on a schedule it
controls — never once per capture, never a second concurrent STA path. Under `SAPPER_TEST_HOOKS` the
sync is driven by the already-ledgered clock-advance stimulus (LEDGER: serial control STIMULUS half) so
the on-air verify need not wait a real hour; that vocabulary stays out of any shipped binary (§4
invariant #4).

**7. The sync is headless-first: it emits events, it renders nothing.** slice-6 raises the
new-password event, the first-sync summary event, and a sync-outcome event (counts: downloaded, new,
malformed, and network result); it depends on no display, LED, or web surface. slice-7 builds those
surfaces as subscribers. This holds ADR-0001's rule that every surface is an event-bus subscriber, so
the same sync engine runs on a board with no screen.

## Consequences

- **A new §4 standing invariant is added in the same commit as this ADR:** cracked passwords are read
  and written only through the `CrackedManifest` seam — no surface reads a results file directly. This
  mirrors invariant #9 (captures only through `CaptureSink`) and #6 (secrets only through the
  provisioning seam), and exists so slice-7's several surfaces cannot each grow their own parse of the
  store. It is `review-only`, like its siblings.
- **The cracked passwords are a new plaintext-at-rest secret surface,** under the *same* deferred
  flash-encryption item ADR-0006 already ledgers for provisioned secrets — a flash dump reveals recovered
  PSKs until that lands. No new ledger line; the existing deferral now also covers the manifest, noted
  where it is closed.
- **This completes the capture→upload→sync→alert spine (slices 1–6).** After this, the appliance is
  functionally whole: it hunts endlessly, uploads, and announces cracks. What remains on the roadmap is
  actuation (deauth), the alert *surfaces* (slice-7), and broadening hardware (slice-8) — additions to a
  working loop, not gaps in it.
- **No shipped actuation surface is added.** The fetch is an authenticated *read* of the operator's own
  account over the same client-mode TLS the upload and NTP steps already use; it is not the deauth/frame
  injection §4 invariant #4 gates. The only test-only vocabulary is the clock-advance sync stimulus,
  already gated behind `SAPPER_TEST_HOOKS` and ledgered.
- **The reference's two defects are recorded as rejected-and-fixed, not silently improved:** the
  colon-in-password truncation (decision #3) and the device-only result filter (decision #2). Writing
  *why* they were wrong here means a future reader porting more from the Adversary meets the reasoning,
  not a mystery divergence (stele:ADR-0012).
