---
id: '0108'
title: "On-device WPA*01 hc22000 synthesis from the whole captured frames (pwncrack feed)"
type: architecture
status: accepted
date: 2026-11-02
supersedes: []
superseded_by: []
---

## Context

The Sapper uploads a raw `.pcap` to wpa-sec, which runs `hcxpcapngtool` server-side to recover the
nonces/MIC and build the hashcat `22000` hashline itself (ADR-0017). The decided second cracking
backend is **pwncrack.org** (LEDGER), and pwncrack does **not** take a `.pcap`: it takes a hashcat
`.hc22000` hashline. So a second backend forces the device to synthesise the hashline that wpa-sec's
server does for us today. This ADR fixes how, before any uploader consumes it.

Two constraints already in the corpus shape the answer:

- **The capture core deliberately parses no nonces or MIC (ADR-0009 decision #3).** `HandshakeCollector`
  keeps the beacon and M1-M4 as **whole 802.11 frames** (`CapturedHandshake.msg[0..3]`, M1..M4) and
  extracts nothing from them, because `hcxpcapngtool` recovers those fields from the frames, so on-device
  extraction was dead work. A pcap of whole frames is all wpa-sec ever needed.
- **All 802.11/EAPOL interpretation lives in the pure capture core, host-tested (§4 #8).** The sniffer
  seam forwards raw bytes only; no protocol logic may leak onto a device seam.

The sibling **The Adversary** already generates hc22000 on-device
(`adversary:src/utils/handshake_save.h` `buildEapolHc22000Line`), but in a shape that does not fit here
and carries a latent defect:

- It reads from a `CapturedHandshake` whose nonces/MIC/addresses were **parsed at capture time** into
  struct fields. Our struct holds no such fields (ADR-0009 #3), so a verbatim port is impossible.
- It is **not pure**: the builder sits in a header beside `SD`/`File` IO (`writeHandshake22000`), so its
  802.11 reading never runs on a host test lane. That is the §4 #8 violation this repo forbids, and the
  Adversary never tested the emitted line against a known vector (its own code comments concede the
  service wording "isn't documented").
- It sources the ANonce from "M1 **or** M3" but always tags `messagepair 00`. This is **not** a crack
  defect: the AP selects one ANonce per handshake instance and sends that same value in both M1 and M3,
  so an M3-sourced ANonce is byte-identical to M1's and the line cracks. It is at most a cosmetic label
  (an M3+M2 capture tagged as the M1+M2 pair). We source from M1 for a different reason, below.

Everything a WPA*01 line needs is recoverable from the whole frames we already keep, at build time,
with no change to the collector and no capture-time parsing:

| hc22000 field | Source in our whole frames |
|---|---|
| MIC (16) | M2 EAPOL offset 81 (the Key MIC field) |
| AP BSSID (6) | `CapturedHandshake.bssid` |
| station MAC (6) | M2 Addr2 (M2 is client→AP, so Addr2 is the station; frame offset 10) |
| ESSID | `CapturedHandshake.ssid` (from the beacon) |
| ANonce (32) | M1 EAPOL offset 17 (the Key Nonce field) |
| EAPOL blob | the M2 frame's EAPOL payload, located via `locateEapol`, MIC field zeroed |

The EAPOL-Key layout giving offsets 17 and 81: 4-byte EAPOL header, then descriptor type (1), key
information (2), key length (2), replay counter (8), **key nonce at 4+1+2+2+8 = 17** (32), key IV (16),
key RSC (8), key ID (8), **key MIC at 81** (16), key-data length (2) — so a 99-byte EAPOL body is the
minimum that holds the MIC and the length word (matching the parser's existing `kEapolMinKeyLen`).

## Decision

1. **Add a pure module `net/hc22000` (`buildHc22000Line`)** that takes our `CapturedHandshake` (whole
   frames) and writes a NUL-terminated hashcat **WPA\*01** line into a caller buffer, extracting the
   fields from the frames at build time via the existing `locateEapol` seam. It lives on the native lane
   (`build_src_filter`) and is host-tested against a crafted M1/M2 with known nonces/MIC. No field is
   extracted in the collector; ADR-0009 #3 is preserved, and §4 #8 holds (the reading is pure and tested).

2. **Source the ANonce from M1, and require M1.** The wpa-sec validity gate already guarantees
   beacon+M1+M2 (`isWpaSecValid`), so M1 is present whenever a capture is worth sending. Taking the
   ANonce from M1 (not also M3) keeps the code single-sourced and makes `messagepair 00` exactly truthful
   (a genuine M1+M2 pair), rather than labelling an M3+M2 capture as M1+M2 the way the Adversary does.
   This is a clarity/simplicity choice, not a crack-correctness fix: M1 and M3 carry the identical ANonce
   value, so the Adversary's M3 fallback still cracks. The real correctness property is **not confusing
   M2's SNonce for the ANonce** (a different value) — tested. A capture lacking M1 or M2 yields no line.

3. **WPA\*01 (4-way) only; no WPA\*02 (PMKID) in this ADR.** The collector keeps no PMKID (it is in M1's
   RSN key-data KDE, which we do not parse). A full-4-way WPA\*01 line is broader coverage than PMKID
   anyway (not every AP leaks a PMKID). PMKID-from-M1 extraction is a clean later enhancement, deferred
   (LEDGER).

4. **A hidden-SSID capture yields no line (returns false).** WPA\*01 PMK derivation needs the ESSID; an
   empty ESSID produces an uncrackable line. wpa-sec can still crack such a capture from the pcap via its
   own ESSID discovery, so this only scopes the pwncrack feed, loudly (no line), never a silent empty one.
   The empty-check is `strlen`-based because `CapturedHandshake` holds the SSID as a C string with no
   length field (ADR-0009), so an SSID with an embedded/leading NUL reads short and is treated as hidden
   here: a rare, non-destructive upstream data-model limitation deferred to the LEDGER, not re-plumbed by
   this ADR.

5. **The module is read-only over the capture.** It mutates nothing, allocates nothing on the heap (a
   bounded stack copy of the EAPOL to zero the MIC), and emits one line per call. It has no consumer yet;
   the pwncrack `Uploader`/classifier/cracked-sync seams are separate later units, after the pwncrack API
   is re-verified firsthand (LEDGER).

## Consequences

- A new §4 standing invariant (#45) records that hc22000 synthesis is pure, host-tested, sources the
  ANonce from M1, and never parses in the collector, so a future reader does not "optimise" it back into
  capture-time parsing (the ADR-0009 #3 / §4 #8 regression) or emit M2's SNonce where the ANonce belongs
  (the one genuine correctness hazard the test guards).
- The collector, the pcap path, and the wpa-sec upload are untouched: this is additive and pure, so the
  native suite is the whole proof and no on-air re-verification of existing behaviour is needed.
- Two deferrals land in the LEDGER: WPA\*02 PMKID extraction, and the pwncrack uploader/classifier/
  cracked-sync seams (gated on re-verifying pwncrack's current API, not the possibly-stale Adversary copy).

## Amendment — 2026-11-07: hashcat type tag corrected (the 4-way line is WPA*02, not WPA*01)

This ADR, its title, and the shipped `buildHc22000Line` tagged the 4-way EAPOL line **`WPA*01`**. That is
wrong: the hashcat 22000 spec numbers the type field **01 = PMKID, 02 = EAPOL** (confirmed against the
[hashcat wiki](https://hashcat.net/wiki/cracking_wpawpa2) and an hcxtools sample line, 2026-11-07), so a
4-way/EAPOL line is **`WPA*02`** and `WPA*01` is reserved for a PMKID line. The builder emitted the EAPOL
layout (MIC + ANonce + EAPOL blob + messagepair) under the `01` tag, so pwncrack read the M2 MIC as a PMKID
and never cracked it: the whole pwncrack path was uncrackable since it shipped (wpa-sec is unaffected, it
takes the pcap). Corrected to `WPA*02` in `buildHc22000Line`, with the tests asserting the spec tag
(fail-before/pass-after) and §4 #45 updated. Mirrors the sibling `adversary:54b26d8`. The decision this ADR
records (pure, host-tested, on-device synthesis from the whole frames, ANonce from M1) is unchanged; only
the type-tag number was a transcription error. The PMKID line's own tag correction (it is `WPA*01`, not
`WPA*02`) is recorded on ADR-0110.
