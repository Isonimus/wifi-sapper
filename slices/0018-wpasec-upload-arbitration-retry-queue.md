---
id: '0018'
title: "wpa-sec upload — arbitration, persisted retry queue, and the first shipped hunt loop"
type: slice
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Goal

Turn a captured handshake into an accepted wpa-sec upload, and with it start the appliance's first
shipped hunt loop (ADR-0017). A pure retry-queue policy over two new seams — a `CaptureStore` (where
pcaps wait) and an `Uploader` (the TLS POST) — subscribes to the HuntEngine's capture-ready event,
serializes each handshake to the store, and on a batched threshold performs one drain cycle: pause the
hunt, bring the STA up, upload the queue over TLS with a pinned root CA, and resume promiscuous
hunting. The queue survives power loss (LittleFS), is bounded and deduplicated per BSSID, and every
store or upload failure is loud. This closes the ledger's on-air handshake→pcap proof (ADR-0011): the
byte sink now exists.

It does **not** download cracked results or schedule the hourly sync (slice-6 — it shares this drain's
STA session), drive any UI (headless — ADR-0001; the drain result is an event slice-7 attaches to), or
transmit deauth (still deferred, still behind the §4 invariant #4 actuation gate). On air it proves the
capture→store→TLS-upload path with a stimulus-injected handshake; a deauth-forced natural capture waits
on the deauth slice.

## Definition of Done

**Scenario A — a capture-ready event is enqueued as a pcap without pausing the hunt (host).**
- **Given** the drain supervisor wired to a fake `CaptureStore`, a fake `Uploader`, and a fake clock,
  below a drain threshold
- **When** the HuntEngine's capture-ready event fires with a wpa-sec-valid `CapturedHandshake`
- **Then** the handshake is serialized (via `serializeHandshake`) into the store as one pending pcap,
  no drain cycle runs, and neither `huntEngine.stop()` nor the uploader is called — the hunt keeps
  running.
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario B — a threshold triggers one batched drain that pauses, uploads all, and resumes (host).**
- **Given** a supervisor with three pending captures for distinct BSSIDs and a fake `Uploader` that
  returns `accepted`
- **When** the pending-count (or time) threshold is reached and it is ticked
- **Then** it stops the engine, waits the quiesce settle on the clock, drains all three through the
  uploader **once each**, deletes each from the store only on the accepted result, and resumes the
  engine — a single associate for the whole batch, not one per capture.
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario C — an unrecognised or failed upload is retried, never silently dropped (host).**
- **Given** a pending capture and a fake `Uploader` that returns `rejected` (or an unrecognised body)
  on the first drain and `accepted` on the second
- **When** two drain cycles run
- **Then** the capture is **not** deleted after the first cycle (it remains pending), the backoff
  grows, and it is deleted only after the second cycle's accepted result — a capture leaves the queue
  only on a positive accepted/duplicate classification (ADR-0017 decisions #2, #4).
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario D — a `duplicate` response is a terminal success (host).**
- **Given** a pending capture and a fake `Uploader` returning `duplicate` (wpa-sec already holds it)
- **When** a drain cycle runs
- **Then** the capture is deleted from the store exactly as for `accepted` — a duplicate is off our
  hands, not an error to retry.
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario E — the queue is bounded and deduplicated per BSSID (host).**
- **Given** a store at `kMaxQueuedCaptures` pending distinct-BSSID captures
- **When** a new capture arrives for a BSSID already pending, then another for a brand-new BSSID
- **Then** the same-BSSID capture **replaces** the pending one (count unchanged, newer bytes kept), and
  the new-BSSID capture evicts the **oldest** pending one (count stays at the bound) — never an
  unbounded queue, never two pending entries for one BSSID (ADR-0017 decision #4).
- **Proof:** `npm run test:native` → `test/test_capture_queue`.

**Scenario F — backoff grows on failure and resets on success, measured by the clock (host).**
- **Given** a supervisor whose drain cycles fail (no association) repeatedly
- **When** it is ticked across several failed cycles and then one that succeeds
- **Then** the interval before each next cycle grows exponentially up to the cap while failing, and
  drops back to the base the tick after a successful cycle — driven purely by the fake clock, no
  blocking wait (ADR-0017 decision #7).
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario G — the pinned root CA validates the live wpa-sec chain and uploads a real pcap (on-air,
lane 3).**
- **Given** a Cardputer flashed with the `cardputer_testhooks` build, provisioned with real WiFi
  credentials and a real wpa-sec key, with a stimulus-injected wpa-sec-valid handshake
- **When** `npm run verify:upload` drives the device
- **Then** the device associates as a station, syncs NTP, opens a TLS connection to
  `wpa-sec.stanev.org` that validates against the **pinned GTS Root R4** (a cert-validation failure is
  a `[FATAL]`), POSTs the serialized pcap, parses wpa-sec's response into accepted/duplicate, deletes
  the capture from LittleFS on success, and returns to hunting (`[HUNT] discovering …`) — proving the
  capture→store→TLS-upload→resume path end to end on real hardware and against the real service. The
  run fails on any `[FATAL]`/`[ERROR]`; the artifact records the `[UPLOAD]` lines and the observed TLS
  handshake / associate / upload timing.
- **Proof:** `scripts/0018-upload-verify.mjs`, wired as `npm run verify:upload`.

## Design

**The pure capture queue.** `src/net/capture_queue.{h,cpp}` — the bounded, per-BSSID-deduplicated
policy over an abstract `CaptureStore`. Owns no bytes and no filesystem: it decides *what* is pending,
*which* to replace, and *which oldest* to evict, and calls the store to enqueue/list/read/delete.
Host-tested through an in-RAM fake store.

**The `CaptureStore` seam.** `src/net/capture_store.h` — `enqueue(bssid, pcapBytes)` /
`listPending()` / `read(id)` / `remove(id)`, each reporting failure (§3). Device backing
`src/net/capture_store_littlefs.{h,cpp}` (device-only, `#ifndef UNIT_TEST`) writes one pcap file per
pending capture under a LittleFS directory; the host lane uses `test/support/fake_capture_store.h`.

**The `Uploader` seam.** `src/net/uploader.h` — `Result upload(const uint8_t* pcap, size_t len, const
char* key)` returning `enum class UploadResult { Accepted, Duplicate, Rejected }`. Device backing
`src/net/uploader_wpasec.{h,cpp}` (device-only) opens a `WiFiClientSecure` with the pinned root CA
(`src/net/wpasec_root_ca.h` — the measured GTS Root R4 PEM, ADR-0017 decision #1), POSTs the
multipart body with the `key` cookie, and classifies the response fail-closed (unrecognised →
`Rejected`). The host lane uses a fake returning a scripted sequence of results.

**The drain supervisor (pure policy + injected seams).** `src/net/upload_supervisor.{h,cpp}` — holds
a `HuntEngine&`, a `CaptureQueue&`, an `Uploader&`, a station-control seam, and the clock. Subscribes
to capture-ready as a `CaptureReadyObserver`: on each event it serializes and enqueues (no radio
change). `tick(nowMs)` runs the drain state machine: below threshold, nothing; at threshold, stop the
engine → wait the quiesce settle → associate + NTP → drain the queue → tear STA down → resume the
engine; on failure, back off (decision #7). All timing is clock-driven and host-tested; the actual
`connectStation`/`syncClock`/`WiFi.mode` calls are behind a thin station-control seam so the pure
state machine is testable without a radio (invariant #2 — stimulus through the same seam).

**The station-control seam.** A minimal interface (`bringUpStation()` / `tearDownStation()` /
`clockIsReal()`) wrapping the existing `wifi_station.h` functions and the promiscuous↔STA mode change,
so the supervisor's sequencing is host-tested against a fake and the device wiring is one thin adapter.

**The shipped loop.** `main.cpp` constructs the real sniffer, engine, queue (LittleFS store), uploader
(wpa-sec), and supervisor once provisioned and past the boot gate (ADR-0006), and pumps
`engine.tick()` + `supervisor.tick()` — the first shipped hunt loop (ADR-0017 decision #8). Under
`SAPPER_TEST_HOOKS` the verify path injects a synthetic handshake through the capture seam so the
on-air upload can be exercised without waiting for a natural capture (this is the STIMULUS half the
ledger tracks, landing with the engine it drives).

## Verification

- **Native Unity tests** (Scenarios A–F) — `test/test_upload_supervisor` (enqueue-without-pause,
  batched drain, retry-on-failure, duplicate-as-success, backoff) and `test/test_capture_queue`
  (bound + per-BSSID dedup + oldest-eviction), driving the pure policy through fake `CaptureStore`,
  fake `Uploader`, a fake station-control seam, and a fake clock. Wired as `npm run test:native`; runs
  in cloud CI (ADR-0004 lane 1). The pinned-CA validation and the real wpa-sec contract are not
  host-assertable and are proved by Scenario G.
- **On-air verify** (Scenario G) — `scripts/0018-upload-verify.mjs`, wired as `npm run verify:upload`
  (R11). Runs on a workstation with a provisioned board attached, never in cloud CI (§4 invariant #5);
  its error-check half (fail on any `[FATAL]`/`[ERROR]`, require a successful TLS handshake against the
  pinned root, an accepted-or-duplicate upload, a capture deleted from the store, and a return to
  hunting) is machine-checkable, and it writes the observed `[UPLOAD]` lines and TLS/associate/upload
  timing as the review artifact.
- **Board compile** (regression) — `npm run build:boards` (`pio run -e cardputer`) links the shipped
  `.elf` with the real hunt loop, uploader, and LittleFS store present (this is the first shipped build
  that hunts and uploads); the device-only seams compile under the ESP toolchain and the pure policy
  shares the native lane. Flash grows by the pinned PEM, the TLS stack already linked for NTP/TLS, and
  the loop; the report records the new baseline.

## As built

Shipped as designed, host-proven on the native lane and board-compiled on both the shipped and
test-hooks builds. Deviations from the plan above, recorded per the freeze step:

- **Station-control seam folds the clock gate.** The seam is `bringUpStation()` / `tearDownStation()`
  only — `clockIsReal()` was dropped. `bringUpStation()` returns true only when the station is
  associated **and** NTP has set a TLS-valid clock, so the supervisor has one honest "TLS-ready" gate
  instead of sequencing two calls (KISS; the design's three-method shape was not needed).
- **Two files beyond the planned list.** The shipped hunt→enqueue→drain→upload spine and the on-air
  verify both need the same wiring, so it lives in one `src/hunt_loop.{h,cpp}` both use, and the verify
  takeover is `src/upload_probe.{h,cpp}` (mirroring the slice-0016 `hunt_probe`), rather than the
  wiring sitting inline in `main.cpp` as the Design sketched.
- **Scenario A–F** (host): met exactly — `test/test_capture_queue` (5 tests: store-once, invalid
  dropped, per-BSSID replace-keeps-newer, oldest-eviction, store-error-surfaced) and
  `test/test_upload_supervisor` (5 tests: enqueue-without-pause, batched-drain-uploads-all-and-resumes,
  rejected-retried-never-dropped-with-growing-backoff, duplicate-as-terminal-success,
  backoff-grows-and-resets). Both drive the pure policy through the fake store/uploader/station and a
  fake clock; the supervisor tests use a real `HuntEngine` over a `FakeRadioSniffer` so stop/resume is
  observed through the engine's phase.
- **Scenario G** (on-air, lane 3): **proven on a Cardputer against live wpa-sec**, with both a
  synthetic stimulus and a real captured handshake. The verify injects a wpa-sec-valid handshake
  through the capture-ready seam (`huntLoopInjectStimulus`, behind `SAPPER_TEST_HOOKS`) so it needs no
  natural (deauth-forced) capture; a real pcap is embedded at build time from `SAPPER_TEST_UPLOAD_PCAP`
  by `scripts/embed_test_pcap.py` into gitignored `src/gen/` (never committed — it can carry a real
  network's capture). Observed: the TLS connection validates against the **pinned GTS Root R4** (a
  cert-pin/transport failure is a logged `[ERROR]` that fails the run), the multipart POST reaches
  wpa-sec with the key cookie, the response parses, and the hunt resumes (`resumeFailed=0`). A real
  handshake (<redacted-ssid>, EAPOL pair count 1) classified **accepted** and was **deleted** from
  LittleFS; the synthetic (uncrackable) classified **rejected** and stayed queued — the full
  accepted→delete and rejected→retry branches, on air. Artifact: `artifacts/0018-upload.drain.txt`.
- **Response classification was rebuilt against the real wpa-sec reply** (a hardware finding — see
  below). It is now a pure, host-tested function, `net/wpasec_response.{h,cpp}` + `test/
  test_wpasec_response` (6 tests over real captured responses), replacing the guessed token match. The
  contract: endpoint `POST /` on `wpa-sec.stanev.org:443`, key in the `key` cookie, pcap in the `file`
  multipart field; `already in database` → Duplicate; **any `written to 22000 hash file` line whose
  count is ≥ 1** (PMKID or EAPOL) → Accepted; everything else (a `: 0` count, `No valid
  handshakes/PMKIDs found`, a truncated read, an unrecognised body) → Rejected, kept and retried, never
  assumed success. A capture wpa-sec extracts nothing from is thus retried, bounded by the queue's
  oldest-eviction — it is never falsely reported accepted and deleted.

### Hardware bring-up fixes (found by the Scenario G lane, invisible to host tests and the synthetic)

The on-air verify surfaced four defects — three of them in the **shipped** path — that no unit test or
the short synthetic response could reach; each is fixed and, where host-testable, regression-tested:

- **Dangling provisioned credentials.** The verify probe passed a stack-local `ProvisioningRecord`;
  the station adapter and supervisor held raw pointers into it, which dangled after the call returned →
  the drain associated with an empty SSID (`ESP_ERR_WIFI_SSID`). Fixed: `hunt_loop` owns a copy of the
  credentials for the device's whole run and constructs the adapter/supervisor from it, so no caller
  can pass a too-short-lived record.
- **SNTP re-init abort in the drain path (shipped).** `syncClock()` re-ran `configTime()` on every
  drain, restarting the SNTP client; its pending DNS callback then fired inside the upload's own DNS
  lookup and aborted in lwIP without the core lock (`sys_untimeout` / "Required to lock TCPIPcore
  functionality!"). Fixed: `syncClock()` is now idempotent (skips `configTime` once the clock is real)
  and stops the SNTP service after syncing, so its background DNS can never collide with an upload.
- **Response truncation (shipped).** The reply was buffered at 2 KB, but wpa-sec returns hcx's verbose
  ~4.5 KB summary, so the success line was cut off and every real accepted upload read as rejected →
  re-uploaded forever. Fixed: read the whole reply (bounded, on the heap) before classifying.
- **Count-blind classifier (shipped, fail-open).** hcx always prints the `written to 22000 hash
  file...: N` label, so matching the label rather than the count N false-accepted 0-result uploads,
  deleting captures wpa-sec extracted nothing from. Fixed by the count-aware classifier above.

### Adversarial re-pass fixes (found by a blind correctness review of the pure core)

A `/wrap-up` adversarial pass (one Sonnet reviewer, blind to intent, given only the diff + CLAUDE.md +
INDEX) surfaced three real defects in the pure, host-testable core — none on the proven happy path, all
in store-failure handling — plus two lower items. Each fix has a fail-before/pass-after regression test
where host-testable:

- **`offer()` could discard the freshest capture (violated decision #4).** It listed the queue for
  reconcile *before* enqueuing, so a listing failure returned `StoreError` with the just-serialized
  handshake never persisted. Reordered to **enqueue-first**, then snapshot (excluding the new id);
  a listing/reconcile failure now surfaces `StoreError` without ever losing the capture. Regression:
  `test_a_list_failure_after_persist_still_keeps_the_capture`.
- **A corrupt queued capture wedged the backoff at its cap forever (decision #7).** A permanently
  unreadable pcap made every cycle "dirty", pinning the retry cadence at ~15 min so fresh captures
  drained only that slowly, indefinitely. The drain now **purges** an unreadable entry (its bytes are
  already lost) as a distinct `purged` outcome, so the cycle reaches clean and backoff recovers.
  Regression: `test_an_unreadable_capture_is_purged_so_the_cycle_stays_clean`.
- **A failed boot-seed stranded prior-run captures.** If `begin()`'s seed listing failed, `activity_`
  stayed 0 and `shouldDrain()`'s `activity_==0` short-circuit suppressed even the time ceiling, so
  captures already on flash never drained until a new one arrived. The seed now keeps the trigger warm
  (`activity_=1`) on a listing failure so the time ceiling re-reads and self-corrects. Regression:
  `test_a_failed_boot_seed_does_not_strand_prior_captures`.
- **Unbounded frame length in the verify probe (test-hooks only).** `injectRealPcap()` could wrap
  `size_t` and infinite-loop on a corrupt embedded pcap; a bound guard now stops instead. (Gated from
  every shipped binary by §4 invariant #4.)
- **Response parsed over the raw HTTP stream — ledgered, not fixed.** The classifier is fed the full
  response (headers + any chunk framing); it is fail-closed and works on the measured live traffic, so
  proper body extraction is deferred as device-only hardening (LEDGER, ADR-0017 decision #2).
</content>
