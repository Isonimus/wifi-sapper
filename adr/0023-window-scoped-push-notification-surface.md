---
id: '0023'
title: "Window-scoped push-notification surface (ntfy/Discord)"
type: architecture
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Context

The Sapper is a headless appliance the operator is *not* watching. The status LED (slice-0022,
ADR-0021) is a local surface — useful only to someone in the room. The one alert that matters when
the box is hunting unattended in a drawer is "a password just came back cracked," and that alert must
reach the operator's phone. slice-7's push-webhook surface is that channel.

Every other surface so far is a *passive* observer: it receives a fact on the `EventBus` (ADR-0021),
updates some in-RAM state, and renders locally on its own `tick()` — no radio, no network. A push
notification is different in one decisive way: **its reaction is a network transmission, and the
appliance only has connectivity inside an STA window.** Two facts about the existing engine force the
shape of this surface, both read from the shipped code, not assumed:

1. **Connectivity exists only inside a supervisor-owned STA window.** The `UploadSupervisor` owns the
   station: it is up only between `bringUpStation()` and `tearDownStation()` inside `runDrainCycle()`
   (`upload_supervisor.cpp`). The hunt is promiscuous the rest of the time — no association, no TCP.

2. **`runDrainCycle()` is synchronous within one `supervisor.tick()`.** Bring-up, uploads,
   `sync_->runInWindow()` — which publishes `NewPassword`/`SyncCompleted` — and tear-down all happen
   inside a single `tick()`. A surface's own `tick()` runs *after* the supervisor's in the app loop
   (`hunt_loop.cpp`), by which point the window is already closed. So a `NewPassword` fires *while the
   station is up*, but a passive surface reacting on its next `tick()` sees the station already down.

A `NewPassword` is, moreover, always produced *by a sync*, which runs *inside* a window
(`cracked_sync.cpp`: the fetch's TLS is torn down before `NewPassword` is published, and
`SyncCompleted` is published last, station still up). So the fact that must be pushed is generated in
exactly the window where a push could be sent — if something transmits before tear-down.

This is a genuinely new kind of surface — one that **observes on the bus but transmits inside a
window** — and it needs a decision recorded before later transmitting surfaces (a future SMS/email
relay) copy the wrong pattern.

Two subordinate forces:

- **The target host is operator-configured and arbitrary.** ntfy (`ntfy.sh` or self-hosted) and
  Discord webhooks are both wanted; the URL lives in NVS. This is the opposite of the wpa-sec upload,
  whose host is *fixed* and pinned to one self-signed root (ADR-0017 decision #1). Pinning one root
  per arbitrary CDN-fronted host (Discord sits behind Cloudflare; roots rotate) would break silently
  on rotation — the wrong trust model here.
- **The payload is a secret-adjacent broadcast.** The operator chose (this slice) to send the ESSID
  and BSSID only — never the plaintext PSK — because a webhook lands the message on a third party
  (Discord) or a push server (ntfy.sh) and in phone notification history, and the appliance already
  refuses to serial-print a plaintext PSK (ADR-0006, hunt_loop `SerialEventLogger`). The BSSID/ESSID
  are already broadcast in the clear by the AP; the PSK is not.

## Decision

Build the push notification as a **window-scoped transmitting surface**: it observes facts on the bus
like any surface, but it transmits inside the supervisor's STA window, driven by the supervisor
exactly as the cracked-results sync already is (ADR-0019 decision #6). Concretely:

1. **Observe on the bus, transmit in a window — split cleanly.** `WebhookNotifier` implements
   `EventSink`: on `NewPassword` it copies a compact `WebhookNotification` (ESSID + BSSID, **no PSK**)
   into a fixed-capacity pending queue and does nothing else — no clock, no network, exactly as the
   LED's `onAppEvent` only updates state. It also implements a new `WindowNotifier` seam
   (`net/window_notifier.h`), a single `flushInWindow()` the supervisor calls while the station is up.
   All the policy (target detection, message rendering, the pending queue and its overflow accounting,
   retry-by-keeping) is pure and host-tested against a fake transport; only the HTTPS POST is device
   code behind the `WebhookTransport` seam.

2. **The supervisor drives the flush inside `runDrainCycle()`, after the sync, before tear-down.** It
   holds an optional `WindowNotifier*` (wired post-construction via `setNotifier()`, mirroring the
   `CaptureReadyRelay` wiring idiom, so the 8-arg constructor does not grow a ninth) and calls
   `flushInWindow()` after `sync_->runInWindow()` and before `tearDownStation()`. Consequences of this
   placement, all deliberate:
   - A password cracked *this* window (its `NewPassword` fired during the sync moments earlier) is
     pushed in the **same** window — zero added latency, no extra STA window opened for it.
   - A push that failed last window is retried in **any** later window (upload drain or hourly sync),
     because the flush runs every window and keeps unsent notifications queued.
   - The notifier does **not** trigger a window of its own. A fresh crack rides the sync window that
     produced it; a retry waits for the next window the supervisor would open anyway. This keeps
     `shouldOpenWindow()` untouched and the radio un-spun (the LEDGER tracks "notifier could request a
     window" as a possible future refinement if retry latency ever matters).

   This is the *same* relationship the supervisor already has with the `SyncSession` — a window-sharer
   the supervisor invokes inside the window, wired to the bus — so it introduces no new kind of
   coupling. The supervisor depends on the abstract `WindowNotifier`, **not** on the webhook or the
   `surface/` layer, keeping the `net → surface` direction clean (dependency inversion).

3. **Rejected: transmit inside `onAppEvent`.** A `NewPassword` (or `SyncCompleted`) fires while the
   station is up, so a surface *could* POST directly from its bus callback. Rejected because
   `EventBus::publish()` is a synchronous, un-guarded fan-out meant for fast reactions (ADR-0021): a
   multi-second blocking TLS POST inside it stalls every later sink's delivery, makes dispatch
   duration unbounded and order-dependent for correctness, and offers no clean place to retry a failed
   send. Driving the flush from the supervisor puts the blocking IO where all the other in-window IO
   already lives (`runDrainCycle`), leaves the bus a fast fan-out, and makes the flush a plain method
   call testable with a fake transport and no bus.

4. **Multi-target, auto-detected from the URL; one NVS field.** The operator stores one webhook URL
   through the `persistProvisioning()` seam (ADR-0006, §4 invariant #6 — a fourth provisioned value,
   same single-writer path). `detectWebhookTarget()` (pure) picks the wire format from the host:
   `discord.com`/`discordapp.com` → a Discord JSON body (`{"content":"…"}`, `Content-Type:
   application/json`); anything else → ntfy (plain-text body, `Title`/`Priority` headers). Message
   rendering — including JSON-escaping an ESSID that may contain quotes or backslashes — is pure and
   host-tested; the device transport is a thin HTTPS POST. An empty or non-`https://` URL disables the
   surface loudly (logged), never blocks boot — the webhook is optional and must never gate the hunt.

5. **Trust the system root-CA bundle, not a per-host pin.** Because the host is arbitrary and
   CDN-fronted, the transport verifies the peer against the ESP-IDF **default full Mozilla root
   bundle** (`WiFiClientSecure::setCACertBundle(_binary_x509_crt_bundle_start, …)`,
   `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y`, ≤200 roots). Measured: the bundle blob
   (`_binary_x509_crt_bundle_start..end` = 68983 bytes) ships in the framework's `libmbedtls.a`, but the
   linker pulls an archive member only when something references it, and nothing did before this slice
   (the wpa-sec path uses `setCACert` with one PEM). So referencing it **adds ~67 KB of flash** — the
   shipped image measured 38.3% → **40.5%** (+~73 KB incl. the webhook code) on the 8 MB part, well
   within budget. This is an incremental, deliberate cost for arbitrary-host trust, *not* the near-free
   reuse a single-pin enjoys — and cheaper than curating and maintaining a hand-picked multi-root pin
   set across CDNs that rotate. A cert that chains to no trusted root fails the handshake loud (a logged
   error, never an unverified fallback — quality bar §3). This is a *deliberate contrast* with ADR-0017
   decision #1, not a weakening of it: a fixed endpoint you control the expectation of gets the tightest
   possible trust (one pinned self-signed root, ~1 KB); an arbitrary, rotating, operator-chosen endpoint
   gets the standard root store (robust to rotation, still fail-loud). A self-hosted ntfy behind a
   private CA is therefore unsupported until its root is in the bundle — recorded in the LEDGER.

## Consequences

- **Adding a *passive* surface stays additive (ADR-0021); adding a *transmitting* one now has a
  recorded pattern.** A future SMS/email/HTTP-relay surface implements `EventSink` **and**
  `WindowNotifier`, is wired to the bus and to `setNotifier()`, and touches no engine control flow.
  The one hard rule: it observes on the bus and transmits in a window — never blocking IO inside
  `onAppEvent`. This is §4 invariant #14, added in this commit.
- **The supervisor gains a third window responsibility (upload, sync, now notify), but no new coupling
  kind.** It depends on the abstract `WindowNotifier`; the webhook depends on the bus. If a *second*
  window-transmitting collaborator ever appears, the rule of three says extract a `WindowConsumer`
  list from the now-two hardcoded window-sharers (sync, notifier) — not before.
- **The PSK never leaves the appliance over the webhook, by construction.** `WebhookNotification`
  holds no password field, so no rendering path can leak it even by mistake — the choice is enforced
  by the type, not by discipline. An opt-in "include PSK" mode (only defensible on a self-hosted,
  trusted ntfy) is a future decision, not this one.
- **A new provisioned value rides the ADR-0006 seam.** `ProvisioningRecord` gains `webhookUrl`,
  written/read only through `persistProvisioning()`/`loadProvisioning()` (invariant #6 unchanged and
  now covering four fields). It is validated *separately* from the association triad
  (`isUsableWebhookUrl`, pure) so a bad webhook URL disables push without ever routing a good triad to
  the portal.
- **On-air proof is per-target.** The verify (`scripts/0024-webhook-verify.mjs`) drives the real
  observe→enqueue→window→POST path against whichever URL the operator provisions and asserts a real
  2xx. Whichever of ntfy/Discord the operator does not exercise on air is a LEDGER deferral, like the
  single-LED path deferral from slice-0022.
- **Retry is best-effort and window-paced, with no per-item backoff.** A persistently failing URL
  re-attempts every window (bounded by the queue size, no spin — windows are already paced). Faster or
  smarter retry is a LEDGER item, deliberately out of scope here (KISS).

This decision builds on ADR-0021 (the bus is the fact source) and ADR-0019 decision #6 (window
sharing), and contrasts deliberately with ADR-0017 decision #1 (fixed-host pin vs. arbitrary-host
bundle). It creates §4 standing invariant #14.
