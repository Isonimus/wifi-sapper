---
id: '0076'
title: "Stream-classify the wpa-sec upload response — no large contiguous heap alloc on no-PSRAM"
type: architecture
status: accepted
date: 2026-10-26
supersedes: []
superseded_by: []
---

## Context

ADR-0017 decision #2 fixed the wpa-sec response *contract*: classify each reply fail-closed into Accepted
(a `written to 22000 hash file` count >= 1), Duplicate (`already in database`), or Rejected (everything
else, including a truncated read), and delete a capture from the queue only on a positive Accepted or
Duplicate. The device wrapper (`uploader_wpasec.cpp`) implemented the *read* behind that contract by
buffering the whole reply into a single `new (std::nothrow) char[16384]` heap allocation, then handing the
NUL-terminated buffer to the pure `classifyWpaSecResponse`. The 16 KB size and the whole-buffer read were
an implementation choice in the `.cpp`, never stated in ADR-0017.

That choice fails on the reference board. The M5Stack Cardputer ADV is an ESP32-S3 with **no PSRAM**
(ADR-0001), and the response read runs *inside* a live `WiFiClientSecure` TLS session, after mbedtls has
taken its large RX/TX buffers. Chasing why the appliance re-captured the same AP forever surfaced the
cause: the 16 KB `new` fails, `readAndClassify` returns `Rejected`, `markSubmitted` (ADR-0065 #30) never
fires, gate C can never engage, and the hunt re-targets, re-deauths, and re-uploads the same network every
cycle. It also blocked the LEDGER's deauth-forced → wpa-sec-valid proof: no upload could ever be observed
Accepted on this board.

Measured on hardware (`scripts/wpasec-response-probe.mjs`, a `SAPPER_PROBE_WPASEC_RESP` build that streams
the reply with no allocation and reports the three deciding numbers; artifact
`artifacts/wpasec-response-probe.txt`):

| sample | largest contiguous block at alloc site | response length | decisive-token offset |
|---|---|---|---|
| 1 | 7668 B | 4346 B | 2115 B |
| 2 | 7668 B | 4344 B | 2113 B |

The largest contiguous block is a rock-stable **7668 B**, less than half the **16384 B** the buffer needs:
the allocation cannot succeed, not marginally but by ~2.1x. The real response is **~4.3 KB** with the
decisive token at **~2.1 KB**, so it does not vary meaningfully in size.

Two fixes were put with their tradeoffs:

- **B — shrink the buffer** to the measured response size. Rejected. To stay fail-closed the buffer must
  hold the *whole* reply (the positive written-count line can sit anywhere in the ~4.3 KB body, not only
  at the first token), so B needs a ~4.9 KB contiguous allocation. That leaves ~2.7 KB of headroom under
  the 7668 B worst-case block measured in one scenario, on the very heap that just proved it fragments
  badly under TLS, and it keeps a large contiguous allocation in the path, the exact fault class. It
  silences the symptom at a lower threshold instead of removing it (quality bar §3, no workarounds).
- **A — stream-classify.** Chosen. Classify the reply as it arrives, with O(1) state and no buffer sized
  to the response, so the read path has no contiguous allocation to fail. The probe already proved this
  works: it streamed the full 4.3 KB through a 512-byte stack buffer and located the token at 2.1 KB.

The binding constraint turned out to be the tiny contiguous heap (7668 B), not response variance, so A is
correct regardless of the response being stable: it removes the fault class rather than lowering its
trigger.

## Decision

**1. The wpa-sec response is classified by streaming, with O(1) state and no allocation sized to the
response.** A pure, host-tested `WpaSecResponseClassifier` consumes the reply in arbitrary chunks via
`feed(data, len)` and yields a verdict from `result()`. It holds only two substring-match counters, a tiny
line-count state machine, and a bounded integer accumulator, no buffer proportional to the body. The
device wrapper reads the TLS reply into a **small fixed stack buffer** (`kResponseChunkLen` = 256 B) and
feeds each chunk, allocating nothing on the heap. The 16 KB `new`, `kMaxResponseLen`, and the `<memory>` /
`<new>` includes are removed.

**2. The ADR-0017 #2 classification contract is preserved exactly, and the streaming form strengthens its
fail-closed clause.** Same verdicts from the same signals: Duplicate on `already in database`, Accepted on
a `written to 22000 hash file` line whose count parses >= 1, everything else Rejected. Duplicate still
outranks Accepted when both appear in one body (wpa-sec can return the hcx summary *and* the dedup line):
the classifier latches Accepted but finalizes Duplicate-first, and reports `terminal()` only once Duplicate
is seen, so the wrapper may stop early on a duplicate but always reads to the end otherwise and catches a
late dedup line. The whole-buffer read was itself the truncation source ADR-0017 #2's "a truncated read is
Rejected" clause anticipated: here it truncated to *zero* bytes on every upload. Streaming removes it.

**3. `classifyWpaSecResponse(const char*)` stays, as a thin wrapper that feeds the whole string to one
`WpaSecResponseClassifier` and returns its result (DRY, rule of three: one classification implementation).**
The existing host tests against real captured responses keep pinning behaviour unchanged, and new tests
feed those same responses in every chunk-boundary split to pin the token-spanning-chunks case the old
whole-string path never had.

## Consequences

- **Removes the fault class.** With no contiguous allocation >= the response in the reply path, the upload
  classification cannot fail on a heap-fragmentation shortage. `markSubmitted` fires on a real Accepted /
  Duplicate, gate C (ADR-0065 #30) engages, the endless re-capture ends, and the LEDGER's deauth-forced →
  wpa-sec-valid proof is unblocked (the chase that found this defect can finish).
- **A new §4 invariant (#35)** records the standing rule so a later "simplification" back to a buffered
  read is caught: the reply is classified by streaming, no allocation sized to the response in the reply
  path. Enforced by the streaming classifier's host tests (chunk-split equivalence); the wrapper's
  stack-buffer feed is device-only, review-only, proven on air by the chain-close verify.
- **Token-across-chunk-boundary handling becomes a correctness requirement** the streaming tests pin: a
  verdict must not depend on how the TLS stack happened to split the bytes.
- **The measurement probe is retired.** It answered its question and the numbers are here and in the
  committed artifact (quality-bar §3: a probe answers once and the number lands in an ADR); the
  `SAPPER_PROBE_WPASEC_RESP` instrumentation, the `cardputer_probe_wpasec` env, and the reader script are
  removed, leaving no scaffolding in the shipped reply path.
- **No change to the TLS transport or the pin (ADR-0017 #1).** Only the response *read* changes; the
  POST, the cert pin, and the cookie auth are untouched.
