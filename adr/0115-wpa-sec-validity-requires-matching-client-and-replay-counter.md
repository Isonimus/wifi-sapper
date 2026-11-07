---
id: '0115'
title: "A 4-way capture is wpa-sec-valid only when M1 and M2 share a client and replay counter"
type: architecture
status: accepted
date: 2026-11-07
supersedes: []
superseded_by: []
---

## Context

`CapturedHandshake::isWpaSecValid()` (net/handshake_collector) reported a 4-way
capture uploadable on **message presence alone**: `hasBeacon() && has(M1) &&
has(M2)`. The collector keeps only the latest M1 and the latest M2 for its target
BSSID (ADR-0009), keyed on nothing but that BSSID, so nothing checked that the
stored M1 and M2 belonged to the *same* 4-way exchange.

A valid WPA 4-way has M2 replying to M1 with the **same** replay counter, both
between the **same** AP and **same** STA. That is the correctness criterion
wpa-sec and hcxtools apply; the device must apply it too before it claims a
handshake, writes a `.pcap`, or (for pwncrack) synthesises a `.22000` line.

Presence-only pairing produces an invalid handshake whenever more than one
exchange is in the air during a capture window:

- **Crowded RF (shipped since v0.1.0):** two stations handshaking on one AP inside
  the ~8 s window can leave clientA's M1 and clientB's M2 stored together. The
  `.pcap` then carries frames from two stations with mismatched replay counters,
  which wpa-sec rejects as "no valid handshakes." Uncommon but real on a busy AP.
- **Clientless-PMKID solicitation (unreleased, ADR-0110):** routine, not rare. We
  solicit an M1 by associating from a spoofed `02:..` STA; if the AP returns an M1
  with no PMKID KDE, that solicited M1 (addressed to our fake STA) pairs with an
  ambient real client's M2 from an unrelated exchange. The sibling Adversary repo
  measured exactly this on a field card (2026-11-07): three captures wpa-sec
  rejected, e.g. `DIGIFIBRA` M1 rc=2 to the solicited STA vs M2 rc=1 from a real
  client `f4:30:b9:..`; `MERCUSYS` M1 rc=2 vs M2 rc=3.

This is distinct from the hashcat 22000 type-tag inversion fixed just before it
(ADR-0108/ADR-0110 amendments, commit `30cf6f5`): that mislabelled otherwise-valid
lines; this is about not assembling an invalid capture in the first place. It
mirrors `adversary:ADR-0145` (`adversary` commit `b868924`).

## Decision

A 4-way capture is wpa-sec-valid only when its stored M1 and M2 demonstrably
belong to one exchange:

1. **Same station** - the station MAC of M1 (the ANonce message, AP->client, so
   the client is the destination Addr1) must equal the station MAC of M2 (the MIC
   message, client->AP, so the client is the source Addr2).
2. **Equal replay counter** - M2's 8-byte EAPOL-Key replay counter must equal
   M1's.

`isWpaSecValid()` becomes `hasBeacon() && <M1,M2 form a pair>`. When the stored
messages do not satisfy this, the capture is not valid: the engine's capture
window does not terminate on it, no `.pcap` is uploaded, and no `.22000` is
synthesised. A solicited M1 with no PMKID and no matching M2 from the same STA
yields nothing, which is correct (`isPmkidValid()`, ADR-0110, is unaffected: a
PMKID is crackable from the lone M1, needs no pairing, and keeps its own path).

**Derived from the whole frames, no new struct fields.** Unlike the sibling repo,
which retains per-message client-MAC and replay fields on its capture struct, the
Sapper stores whole 802.11 frames (`CapturedHandshake.msg[0..3]`, ADR-0009). The
station MAC and replay counter are therefore read **at validation time** in the
pure, host-tested core, through the existing `locateEapol` seam plus two new pure
readers (`stationMac`, `eapolReplayCounter` in net/eapol). Nothing is extracted or
stored in `HandshakeCollector`, so ADR-0009 #3 (the collector parses nothing
on-device) and the §4 #8 pure-core boundary hold exactly as they do for the
ADR-0108 hc22000 synthesis and the ADR-0110 PMKID extraction.

**Scope: M1+M2 only.** The sibling repo also accepts an M2+M3 pair (M3 carries the
ANonce when M1 was missed; M3's replay is M2's + 1) in its broader SD-card save
gate (`isMinimumValid`). The Sapper has no such second gate: `isWpaSecValid` is the
one 4-way terminal/upload condition, it has always required M1 **and** M2 (so an
M2+M3-without-M1 capture was never valid here), and the pwncrack `buildHc22000Line`
reads the ANonce from M1. Accepting M2+M3 would be new capture breadth, not part of
this correctness fix, so it is out of scope and deferred (LEDGER).

## Consequences

- Captures that previously "succeeded" by mixing frames from two stations stop
  being treated as valid: the window keeps hunting for a real pair instead of
  terminating on a rejectable one, and wpa-sec stops receiving uploads it rejects.
  A truer success count, not a regression.
- The pure core gains two small readers (`stationMac`, the DS-direction complement
  of `frameBssid`; `eapolReplayCounter`, the 8-byte big-endian counter at EAPOL
  offset 9). No capture struct grows; the cost is a few pointer reads per
  validation, off the frame-callback path (validation runs on the app task).
- Full correctness in a crowded environment ultimately wants per-client handshake
  tracking (concurrent exchanges on one BSSID accumulated separately). This ADR
  mandates the pairing *invariant* only; a per-client accumulator is a possible
  later refinement, out of scope here.
- An AP that retransmits M1 with an *incremented* replay counter (an EAPOL-Key retry
  on a lossy link, reusing the same ANonce) can momentarily leave the stored M1 (new
  counter) beside an already-stored M2 (old counter); the collector keeps only the
  latest of each (ADR-0009). The check rejects that mix, which is correct: the clean
  crackable pair is messagepair-00 (equal counters), and the client's M2 responding
  to the retransmit then completes a matched pair, so the cost is at most a longer
  capture window on a lossy link, never a lost handshake (the window stays open). A
  future reader must **not** loosen the check to ANonce-equality-only to "rescue" such
  a pair: that readmits the lower-quality nonce-corrected/rogue messagepairs and
  reopens the cross-station mispair this ADR closes.
- Captures already uploaded are unaffected; this governs new captures. Re-capturing
  an affected AP produces a valid handshake.
