---
id: '0059'
title: "Skip uploading captures for already-recovered networks (the CrackedLookup seam)"
type: architecture
status: accepted
date: 2026-09-21
supersedes: []
superseded_by: []
---

## Context

The Sapper hunts **endlessly** and re-captures 4-way handshakes for the same networks on every pass.
Each capture is enqueued and uploaded to wpa-sec once, so a network that stays in range is uploaded
again and again — observed in the field as duplicate submissions (one lab network showed a "Get works"
count of 12 for a single BSSID). This is not a wpa-sec fault: wpa-sec **deduplicates the cracking**
(it will not re-crack a handshake it already holds) but still **accepts** each upload — it does not
bounce a duplicate. So duplicates do not corrupt results, but on a headless, endless-hunt appliance they
waste the scarce things: STA-window airtime, TLS uploads, the bounded on-flash capture queue
(`kMaxQueuedCaptures = 32`), and the operator's attention.

The appliance already **knows** which networks are done. Recovered PSKs are persisted in the
`CrackedManifest` (ADR-0019, §4 #12), keyed by BSSID, with a by-BSSID `find()` read seam. On the hunt
path the manifest is a live, populated global (`g_manifest`, `hunt_loop.cpp`), refreshed each hour by
`CrackedSync`. So "have we already recovered this BSSID's PSK?" is answerable locally, for free, at the
moment a capture is about to be uploaded — where the BSSID is already in hand (`PendingCapture.bssid`,
`upload_supervisor.cpp` drain loop).

There are **two** places redundant work happens: the upload (this ADR, gate A) and the capture/targeting
itself (deferred to its own ADR, gate B — skipping a cracked AP in `HuntEngine` target selection). Both
want the same question — "is this BSSID already cracked?" — so the question, not the whole manifest, is
what to depend on.

## Decision

1. **A narrow read-only lookup seam, `CrackedLookup`.** A pure abstract interface —
   `virtual bool isCracked(const uint8_t bssid[6]) const = 0;` — in its own header (`net/cracked_lookup.h`),
   implemented by `CrackedManifest` over its existing `find()` (`isCracked(b) == (find(b) != nullptr)`).
   Consumers depend on the *question*, not on `CrackedManifest`'s 28 KB concrete type — dependency
   inversion that earns its keep because there are **two** consumers (this ADR's uploader and gate B's
   engine), and it keeps both host-testable against a trivial fake. Reads go only through this seam over
   the §4 #12 `CrackedManifest` read path — never a parallel manifest parse (§4 #12 preserved).

2. **Gate A — the `UploadSupervisor` drops an already-cracked capture instead of uploading it.** In the
   drain loop, before reading the pcap bytes and calling `Uploader::upload`, the supervisor consults the
   lookup for `pending[i].bssid`; if it is already cracked, the entry is `remove()`d from the
   `CaptureQueue` and **not** uploaded, counted in `DrainOutcome.skippedCracked` and surfaced on the bus
   in `DrainCompleted` exactly as `purged`/`accepted` are (this drain module is pure and does no Serial
   IO — the outcome is the record, not a log line). Removing it — rather than merely skipping — frees the
   bounded queue and stops the capture re-appearing every drain.

3. **Wired by a post-construction setter, off by default.** `UploadSupervisor::setCrackedLookup(const
   CrackedLookup&)`, mirroring `setNotifier` (the constructor deliberately does not grow a ninth
   argument). Unset (`nullptr`) → the exact slice-0018 drain behaviour, so every existing upload test and
   an upload-only build are unchanged. `hunt_loop` wires `g_manifest` (which now implements
   `CrackedLookup`) in.

4. **Gate on knowledge, never hope.** Only a BSSID *actually present in the recovered manifest* — i.e.
   wpa-sec cracked it and `CrackedSync` brought it back — is skipped. A network we have captured but not
   yet cracked still uploads, because that upload is how it gets cracked. So the gate naturally engages
   only *after* the hourly sync lands the crack; the first capture of a network is never suppressed.

5. **Gate B is deferred to its own ADR/slice.** Skipping a cracked AP in `HuntEngine` target selection
   reuses this same `CrackedLookup` seam but gives the engine its first read-only persisted-state input,
   a boundary change worth its own decision record. It is not made here; this ADR ships gate A only.

## Consequences

- The endless-hunt appliance stops re-uploading handshakes for networks it has already recovered — the
  observed duplicate-submission behaviour. It still uploads the first, cracking-worthy capture of every
  network, and still announces genuine new captures/cracks on the bus (surfaces unaffected).
- **New §4 invariant #27**, added in this commit: cracked-BSSID captures are dropped-not-uploaded via the
  `CrackedLookup` seam. Gate B will extend the row (or add its own) when its ADR ships.
- `CrackedManifest` gains a base class (`CrackedLookup`) and one trivial override; no behaviour change to
  its existing users. The manifest read stays inside the §4 #12 seam.
- Edge: manifest eviction (`kMaxCrackedEntries = 256`, the ADR-0019 LEDGER bound). If a cracked entry is
  evicted, its network becomes un-skipped and may be captured/uploaded once more until the next sync
  re-lands it — bounded and self-correcting, not a leak.
- **Accepted limitation — a rotated PSK is not recovered while its BSSID stays in the manifest.** Once a
  BSSID is recovered, `isCracked` is true for as long as the entry lives, so gate A drops *every* later
  capture of it. If that network's operator rotates the PSK, the appliance never re-uploads the new
  handshake, so wpa-sec never cracks the new key and the `CrackedManifest` `Changed` path (ADR-0019 #5,
  which exists for exactly this rotation case) can never fire for it — the surfaces report the stale old
  PSK until the entry is LRU-evicted (unrelated to freshness). Before this ADR, the endless re-uploads
  caught rotations naturally; gate A trades that recovery away for the airtime/queue saving, which is the
  right trade for an appliance whose job is breadth of *first* cracks and whose scarce resource is STA
  airtime — a mid-engagement PSK rotation is the uncommon case. This is a **recorded decision, not a
  silent gap** (§2): the fully self-correcting fix — re-verifying a cracked BSSID at most once per long
  interval (a per-entry last-verified timestamp + a drain-clock gate, so the constant re-uploads stay
  suppressed but a stale entry eventually re-uploads) — is deferred to its own slice (LEDGER), not built
  here, to keep gate A minimal.
- Deferred: gate B (skip capturing/targeting a cracked AP) — LEDGER, its own ADR/slice. A bounded
  re-verification cadence for rotated PSKs — LEDGER, its own slice.

## Alternatives considered

- **Dedup identical re-captures by content/BSSID in the queue** (regardless of cracked state): narrower —
  it would collapse repeat captures of an *uncracked* network too — but it does not stop the real waste
  (re-uploading networks we are already done with) and adds a per-BSSID "seen" ledger the manifest already
  is. Rejected in favour of the manifest gate.
- **Injecting the concrete `CrackedManifest` into the supervisor** instead of a narrow interface: simpler
  by one header, but couples the upload path to the 28 KB type and is harder to fake in a host test, and
  gate B would repeat the coupling. The `CrackedLookup` interface pays for itself across both consumers.
- **Letting wpa-sec absorb the duplicates** (status quo): correct results, but wastes the appliance's
  airtime/TLS/queue on an endless hunter — the cost this ADR exists to remove.
