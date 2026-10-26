---
id: '0077'
title: "Stream-classify the wpa-sec upload response — remove the no-PSRAM 16KB alloc"
type: slice
status: accepted
date: 2026-10-26
supersedes: []
superseded_by: []
---

## Goal

Fix the defect that made the reference appliance re-capture the same AP forever and blocked the
deauth-forced → wpa-sec-valid proof. `readAndClassify` (uploader_wpasec.cpp) buffered the whole wpa-sec
reply into a single `new (std::nothrow) char[16384]`; on the no-PSRAM ESP32-S3, under a live TLS session,
the largest contiguous heap block is only **7668 B** (measured, `artifacts/wpasec-response-probe.txt`), so
the 16 KB allocation fails, the reply is misclassified `Rejected`, `markSubmitted` (ADR-0065 #30) never
fires, gate C can never engage, and the hunt re-targets, re-deauths, and re-uploads the same network every
cycle.

Replace the whole-buffer read with a streaming classification (ADR-0076): a pure, host-tested
`WpaSecResponseClassifier` consumes the reply in arbitrary chunks with O(1) state, and the device wrapper
feeds TLS into a small fixed stack buffer, allocating nothing on the heap. The ADR-0017 #2 classification
contract is preserved exactly.

This unblocks (does not by itself close) the remaining on-air proof in the deauth ledger item
(deauth-forced 4-way handshake → wpa-sec-valid pcap): until this fix, no upload on this board could ever be
observed Accepted. It does **not** close the raw-HTTP-stream de-chunking ledger item: streaming keeps the
same fail-closed behaviour on a token split by chunk framing (missed → Rejected → retry), it does not strip
headers or de-chunk.

## Definition of Done

- **Given** any real wpa-sec reply fixture (accepted EAPOL, accepted PMKID, zero-count, no-handshakes,
  duplicate)
- **When** it is classified through the streaming `WpaSecResponseClassifier`, fed in *any* chunk-boundary
  split (one byte at a time, split across a token, split across the count digits)
- **Then** the verdict equals the whole-string `classifyWpaSecResponse` verdict for that fixture

- **Given** a reply body that contains both `already in database` and a `written to 22000 hash file` line
  with a positive count
- **When** it is classified
- **Then** the verdict is Duplicate (duplicate outranks accepted, in any feed order)

- **Given** the shipped (non-probe) firmware on the no-PSRAM Cardputer ADV, provisioned, armed, and holding
  a real captured handshake for an authorized network not yet in wpa-sec
- **When** the supervisor drains the capture to the live wpa-sec
- **Then** the reply classifies Accepted or Duplicate (not the former alloc-failure Rejected), the serial
  shows `[UPLOAD] wpa-sec result=accepted|duplicate`, `markSubmitted` records the BSSID, and that BSSID is
  not re-targeted on the next enumeration (gate C engages)

## Design

Approach and the measurement behind it are in ADR-0076 (fork A vs B, the 7668 B / 4.3 KB / 2.1 KB numbers).

**What existing code was considered.** The pure classifier `classifyWpaSecResponse` (net/wpasec_response.cpp,
already native-lane tested against real captured responses) is kept and becomes a thin wrapper over the new
streaming classifier, so its behaviour and its whole corpus of fixtures stay the pinned reference (DRY, one
classification implementation; rule of three). The streaming matcher mirrors the existing `countOnLine`
semantics exactly (from just after the `written to 22000 hash file` label, the first digit run on that line,
parsed and compared >= 1) so no verdict changes. The device read loop in `readAndClassify` is reused almost
verbatim, only the destination changes from a 16 KB heap buffer to a 256 B stack chunk fed to the
classifier; the idle-timeout and connected/available loop are unchanged. The `SAPPER_TEST_HOOKS`
`respBytes` log is kept (it now reports bytes streamed). The measurement probe
(`SAPPER_PROBE_WPASEC_RESP`, the `cardputer_probe_wpasec` env, `scripts/wpasec-response-probe.mjs`) is
removed after measurement (quality-bar §3: a probe answers once; the number is in the ADR and the committed
artifact).

The classifier is a small state machine: a duplicate substring matcher run on every byte (outranks all,
makes the verdict terminal), and a written-count matcher that, on completing the label, scans the rest of
the line for the first digit run and latches Accepted on a count >= 1. Both match counters persist across
`feed()` calls so a token or a count split across TLS chunks is still matched. O(1) state, no buffer sized
to the response, which is the whole point (ADR-0076 #1).

## Verification

- **test/test_wpasec_response** (native lane): the existing whole-string fixtures still classify correctly
  through the wrapper; new cases feed each fixture in every chunk split (1-byte, split-on-token,
  split-on-count) and assert the verdict is split-invariant; a new fixture carries both `already in
  database` and a positive written count and asserts Duplicate (precedence, any order). Proves DoD
  scenarios 1 and 2.
- **On-air chain-close** (ADR-0004 lane 3, by hand on attached hardware, artifact committed): flash the
  shipped `cardputer` build, let it capture + drain against the live wpa-sec, and record that the reply
  classifies Accepted/Duplicate, `markSubmitted` fires, and the BSSID is suppressed next cycle (gate C).
  Proves DoD scenario 3. This is also the deauth ledger item's remaining on-air proof.

## As built

Shipped the streaming classifier as designed. `net/wpasec_response` now exposes a `WpaSecResponseClassifier`
(streaming, O(1) state) with `classifyWpaSecResponse(const char*)` kept as a thin wrapper over it;
`uploader_wpasec.cpp` `readAndClassify` streams the TLS reply into a 256 B stack buffer (`kResponseChunkLen`)
and feeds the classifier, allocating nothing on the heap. The 16 KB `new`, `kMaxResponseLen`, and the
`<memory>`/`<new>` includes are gone. The probe (`SAPPER_PROBE_WPASEC_RESP`, the `cardputer_probe_wpasec`
env, `scripts/wpasec-response-probe.mjs`) was removed; the measurement lives in ADR-0076 and
`artifacts/wpasec-response-probe.txt`.

**Host lane (DoD 1, 2):** `test/test_wpasec_response` extended to 9 cases — split-invariance across chunk
sizes {1,2,3,5,7,13,64,4096}, duplicate-outranks-positive-count in both orders, and a prefix-collision
regression. Full native suite 335/335; shipped `cardputer` build green; lint 0/0.

**Adversarial pass (Sonnet, blind to intent).** Two findings, both fixed before this froze:
- The first substring matcher (a naive first-char restart) silently missed a real `already in database`
  after a self-overlapping prefix (e.g. `alre` + `already…`), returning Rejected where the contract requires
  Duplicate. Independently reproduced (old matcher `found=0`, `strstr found=1` on `"alrealready in
  database"`). Replaced with a correct KMP matcher (`detail::TokenMatcher`, failure table bounded by a
  `static_assert` on the two fixed tokens). Regression test `test_duplicate_detected_despite_prefix_collision`
  fails on the old code, passes on the new.
- The device read loop could busy-spin (no yield, no timeout progress) when `available()` was true but
  `read()` returned 0/negative, a watchdog-reboot shape. Restructured so any `r <= 0` takes the
  yield/idle-timeout path, and restored a total-bytes ceiling (`kMaxResponseBytes` = 32768) the old 16 KB
  buffer implicitly provided. Device-only I/O glue (no `WiFiClientSecure` host fake), so review-only +
  proven by the on-air run.

**On-air chain-close (DoD 3, ADR-0004 lane 3, M5Stack Cardputer ADV).** Flashed the shipped `cardputer`
build; a drain classified the wpa-sec reply as a terminal success and the device HUD showed `up:1 cracks:0`
(the `up` counter is `accepted + duplicate` only — `screen_toast_surface.cpp` — so it can only advance on a
terminal success, never the former alloc-failure Rejected which held it at 0), and the previously
re-captured-forever AP is no longer targeted (gate C / `markSubmitted` engaged). No `wpa-sec response buffer
alloc failed` and no `[FATAL]` observed. The exact `[UPLOAD] wpa-sec result=` serial line was not captured
(the drain landed during boot, before the serial observer attached, and gate C then correctly prevented any
re-upload), but the `up:1` counter semantics and the gate-C suppression together are the terminal-success +
`markSubmitted` evidence. This is the proof-of-upload half of the deauth ledger item (deauth-forced →
wpa-sec-valid pcap).

Does **not** close the raw-HTTP-stream de-chunk ledger item: streaming kept the same fail-closed behaviour
on a token split by chunk framing. That remains open.
