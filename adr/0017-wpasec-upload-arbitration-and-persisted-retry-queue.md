---
id: '0017'
title: "wpa-sec upload — TLS transport, promiscuous↔STA arbitration, and a persisted retry queue"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

The HuntEngine (ADR-0015) captures wpa-sec-valid handshakes endlessly and raises each through a
capture-ready event, but nothing consumes that event: a captured handshake has nowhere to go, which is
why ADR-0015 decision #8 ships no hunt loop at all. This ADR records the shape of the component that
closes that gap — the thing that turns a capture into an accepted upload at `wpa-sec.stanev.org` — and
with it the shipped hunt loop becomes possible for the first time.

Four forces shape it:

- **Uploading needs the radio in station mode; hunting needs it promiscuous. They are mutually
  exclusive, and ADR-0013 decision #6 named the HuntEngine layer as the owner of the switch, then
  ADR-0015 decision #7 deferred building it to *here*** — because only here does the STA-mode caller
  (the uploader) exist to test the switch against. The engine already stops cleanly (`stop()` aims its
  atomic router at `nullptr`; §4 invariant #11), so the arbitration is a matter of *when* to pause the
  hunt, bring STA up, and resume — not a new concurrency primitive.

- **The appliance is portable and endless, so it is offline for long, unpredictable stretches** — out
  of AP range, between power cycles, mid-move. A capture taken offline must survive until connectivity
  returns, including across a power loss (a portable device is unplugged as a matter of course, not as
  a fault). An in-RAM-only queue would silently discard every capture gathered while offline the
  moment the battery dies — the exact data-loss a fail-loud appliance must not have.

- **wpa-sec is a public internet service reached over a shared network, and the request carries the
  operator's wpa-sec key.** A plaintext upload exposes both the captured handshake and the key to any
  on-path observer. The transport must authenticate the server (TLS), and — this being a fixed,
  single-endpoint appliance — it can pin far more tightly than a general-purpose browser.

- **Headless-first (ADR-0001).** The uploader is an event subscriber to the engine's capture-ready
  seam; it drives no UI. Its own results (accepted / duplicate / newly-cracked) are themselves events
  that slice-6 (sync) and slice-7 (alert surfaces) subscribe to.

The design forks were put to the operator with their tradeoffs; the decisions below record the choices
made (LittleFS floor, batched drain, measured root-CA pin) and *why*, so later work does not
re-litigate them from nobody's memory.

## Decision

**1. Upload transport is HTTPS with a single pinned root CA, measured from the live endpoint, not a
CA bundle.** The device trusts exactly one certificate: the self-signed **GTS Root R4** (Google Trust
Services; `subject == issuer`, ECDSA P-384, a 765-byte PEM, valid 2016-06-22 → **2036-06-22**). This
was chosen by measurement, not assumption — the live chain on 2026-09-18 was:

| Cert | Subject | Note |
|---|---|---|
| leaf | `CN=stanev.org` | valid 2026-07-26 → 2026-10-24 — a **~90-day** auto-rotated cert |
| intermediate | `GTS WE1` (Google Trust Services) | |
| root (as presented) | `GTS Root R4`, **issued by GlobalSign** | cross-signed, expires **2028-01-28** |
| root (trust anchor, pinned) | `GTS Root R4`, self-signed | valid to **2036-06-22** |

`openssl verify -CAfile <self-signed GTS Root R4>` against the live leaf chain returns `OK`, so the
pin validates the real chain end-to-end. Pinning the *root* — not the leaf, not the cross-signed root
the server happens to present — is forced by the measurement: the leaf rotates quarterly (pinning it
bricks uploads every 90 days) and the presented cross-signed root expires in 2028 (pinning it bricks
uploads in ~16 months), while the self-signed anchor is good for a decade. A CA *bundle* was rejected:
it spends meaningful flash on hundreds of roots this fixed-endpoint appliance will never contact, for
resilience (arbitrary CA rotation) a single-endpoint device does not need. The accepted cost of the
tight pin is that if wpa-sec ever migrates off Google Trust Services, TLS fails **loud** (a logged
handshake failure, not a silent fallback — quality bar §3) and the fix is a firmware update; that is
recorded here so a future operator meets a *documented* failure, not a mystery. Because a pinned cert
cannot be validated against a 1970 clock, upload is gated on a real NTP time — the gate `syncClock()`
already exists for and `wifi_station.h` already documents ("certificate validity cannot be checked with
a clock still at the 1970 epoch"). `setInsecure()` (no validation) was rejected outright: it would let
any on-path attacker capture both the handshakes and the operator's key.

**2. The wpa-sec contract is a strict, fail-loud parse, and its exact tokens are pinned by the on-air
verify against the live service, not guessed here.** An upload is a `multipart/form-data` POST carrying
the pcap in the `file` field with the operator's key presented as wpa-sec's `key` cookie, per wpa-sec's
documented upload API; the response is short text. The uploader classifies each response into exactly
one of **accepted** (a fresh capture wpa-sec took), **duplicate** (wpa-sec already holds it — also a
terminal success: the capture is off our hands), or **rejected/error** (anything else — a non-2xx
status, a body matching no known success token, a truncated read). An **unrecognised** response is an
error, never an assumed success (§3): a capture is deleted from the queue only on a *positive* accepted
or duplicate classification, so a wpa-sec change of wording fails loud and retries rather than silently
dropping captures. The exact endpoint path and the success/duplicate tokens are implementation
constants confirmed by the lane-3 verify (decision below) run against the real service with the
operator's own key — the ADR pins the *contract shape and the fail-closed rule*, the verify pins the
*bytes*, because the bytes are only knowable against the live endpoint and only the operator holds a
key to reach it.

**3. Capture bytes persist behind a `CaptureStore` seam, backed on-device by LittleFS on internal
flash — the portable floor — with SD deferred.** The pure retry-queue logic talks only to an abstract
`CaptureStore` (enqueue a pcap, list pending, read one, delete one), mirroring the `CaptureSink` seam
`pcap.h` already leaves open; a host test backs it with an in-RAM fake, exactly as `FakeRadioSniffer`
backs the sniffer seam (ADR-0004 lane 1). The device backing is LittleFS on internal flash, chosen
over the two alternatives on headless-first grounds: **RAM-only** loses the whole queue on the routine
power loss of a portable device (rejected — the data-loss the second force forbids); **SD** cannot be
the floor because ADR-0001 requires the firmware to run on boards with no SD slot, so requiring it
would break the headless-first promise. LittteFS is present on every ESP32 target, survives reset, and
the pcaps are small and few (a handshake is at most six frames, each ≤ `kMaxFrameLen`) so flash wear
stays low — and wpa-sec deduplicates, so even a re-upload after an ambiguous failure costs nothing but
a little airtime. SD as an optional larger/overflow backing behind the *same* seam is recorded as a
ledger item for slice-8 (broaden hardware), not built now (speculative capacity — §3).

**4. The queue is bounded and content-deduplicated per BSSID; overflow drops the oldest, and every
store failure is loud.** The store holds at most `kMaxQueuedCaptures` pending pcaps. At most one
pending capture exists per BSSID: a fresh capture for a BSSID already queued *replaces* the queued one
(a later capture may hold more of M1–M4 — the newer is at least as complete), which also stops an AP
hunted every discovery round from flooding the queue with copies of itself. When the queue is full of
*distinct* BSSIDs, a new capture evicts the **oldest** pending one: an endlessly-offline appliance must
bound its flash use, and with wpa-sec deduplicating server-side no un-uploaded capture is more precious
than another, so bounding by recency keeps the queue moving rather than freezing on a full disk. A
LittleFS write, read, or delete that fails (full, corrupt, absent) is surfaced as an error the drain
loop counts and logs — never swallowed, never a capture silently reported uploaded that was not (§3).
One refinement to the *read* path: a capture whose bytes cannot be read back is unrecoverable — the
pcap is already lost — so keeping it would let a single corrupt entry pin the queue "dirty" forever.
The drain instead **purges** it, counted and logged loud as a distinct `purged` outcome (never conflated
with an upload); this keeps the read-error path fail-loud without letting it wedge the drain (decision #7).
Persist is ordered **enqueue-first**: the new pcap is written to flash before the queue is listed for
dedup/eviction, so a listing or reconcile failure can never discard the freshest capture in hand.

**5. Arbitration is a batched drain owned by a supervisor above the engine, not a per-capture
switch.** A new component sits above the HuntEngine and the uploader. It subscribes to the engine's
capture-ready event and does nothing there but serialize the handshake through `CaptureStore`
(enqueue) — cheap, no radio change, the hunt keeps running. Only when the queue is non-empty **and** a
threshold is met (a pending-count ceiling **or** a time-since-last-drain ceiling, whichever first) does
it perform one drain cycle: (a) `huntEngine.stop()` — which aims the engine's atomic router at
`nullptr` and stops the sniffer (§4 invariant #11); (b) wait the same bounded quiesce settle ADR-0015
decision #4 uses, so no promiscuous callback is in flight before the Wi-Fi mode changes; (c)
`connectStation()` + `syncClock()` if the clock is not yet real (both from `wifi_station.h`); (d) drain
the queue through the uploader, deleting each capture only on an accepted/duplicate classification; (e)
tear the STA down and `huntEngine.begin()` to resume promiscuous hunting. **Per-capture** upload was
rejected: an STA associate is multiple seconds, so switching modes on every capture-ready event would
spend most of the appliance's life associating and re-associating, missing the handshakes it exists to
catch. Batching amortizes the associate over the whole queue and thrashes the radio least. The whole
switch reuses the engine's existing clean-stop and quiesce discipline — it introduces no new way to
tear a consumer off a live sniffer (invariant #11 stands).

**6. The drain result is itself an event; the supervisor uploads but does not sync, alert, or connect
on its own schedule.** Each accepted upload (and, in slice-6, each cracked-password discovery on the
same STA session) is raised as an event the sync and alert layers subscribe to (ADR-0001). The
supervisor owns *upload* arbitration only; the hourly cracked-results sync (slice-6) shares the STA
session the drain already brings up rather than opening its own, but that scheduling is slice-6's to
decide — this ADR fixes only that the drain and the sync are the two things a single STA window serves,
so the appliance associates on a schedule it controls, not once per capture.

**7. Backoff between drain cycles is bounded-exponential on failure and reset on success.** A drain
cycle that cannot associate or whose uploads all fail (offline) does not retry immediately — it backs
off exponentially up to a cap, so a device carried out of range does not burn power and airtime
associating in a tight loop, and resets the backoff the moment a cycle succeeds. Backoff reflects only
the *network* outcome — a failed associate or a rejected upload; a store read-error is handled by the
purge in decision #4, not by backoff, so a single unrecoverable capture cannot pin the backoff at its
cap and throttle every fresh capture behind it. The backoff schedule is pure, clock-driven, and
host-tested against the fake clock the same way the engine's windows are.

**8. The shipped hunt loop lands here, behind the same provisioning gate the rest of the appliance
uses — no new actuation surface.** With an uploader to receive them, `main.cpp` now starts the real
hunt→enqueue→drain→upload loop once the device is provisioned and past the boot gate (ADR-0006),
outside `SAPPER_TEST_HOOKS`. This is the first shipped code that *transmits* on the STA radio (a TLS
upload), but it is not the deauth/actuation §4 invariant #4 gates: it associates as a client and POSTs
over TLS, the same network use the provisioning and NTP steps already make, and it is driven only by
captures the passive hunt produced. Deauth (active frame injection to force renegotiation) remains
deferred and behind the actuation gate. The on-air verify still drives the loop under
`SAPPER_TEST_HOOKS` with a stimulus-injected handshake so it need not wait for a natural one.

## Consequences

- **The `CaptureStore` seam and the `Uploader` seam are the two new pure boundaries slice-6 builds on**
  — the cracked-results download reuses the same STA session and the same TLS/pin machinery, and the
  same fail-loud response parse. Fixing their shape here means slice-6 subscribes rather than re-plumbs.
- **This closes the ledger's "on-air handshake→pcap proof" deferral (ADR-0011).** The byte sink now
  exists: the lane-3 verify serializes a captured (stimulus-injected) handshake to a pcap through
  `CaptureStore` and uploads it to the live wpa-sec, proving the real emit→store→upload path end to
  end — the proof ADR-0011 said "belongs to the slice that owns the emit/storage path." A *deauth-
  forced natural* handshake on air still waits on the deauth slice; that remains a ledger item.
- **The pinned root CA is a maintenance obligation with a deadline no test can see:** GTS Root R4
  expires 2036-06-22, and wpa-sec could migrate CAs sooner. A TLS handshake failure after a period of
  success is the signal; the fix is re-measuring the chain and updating the pinned PEM in a firmware
  release. This is recorded as a ledger item citing this ADR so the obligation is tracked, not trusted
  to memory — the same discipline the hop-dwell and quiesce-settle measurements follow.
- **No new §4 invariant.** The arbitration reuses invariant #11 (clean stop + quiesce before any radio
  change) rather than adding a rule; the fail-loud upload parse and the single-writer `CaptureStore`
  are quality-bar §3 obligations the review checks, not new repo-specific invariants. Should the drain
  supervisor prove to need a standing rule (e.g. "no upload without a real NTP clock"), it is added
  when built, in the same commit, per the §4 table's own instruction.
- **Provisioning gains no new field:** the wpa-sec key is already in the `ProvisioningRecord`
  (`provisioning_store.h`), read through the one NVS seam (invariant #6). The uploader consumes it
  there; no capture path writes it and no serial command reaches it (invariant #7 stands).
</content>
</invoke>
