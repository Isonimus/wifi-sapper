---
id: '0024'
title: "Push-notification webhook surface (ntfy/Discord)"
type: slice
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Goal

Give the headless appliance a *remote* alert channel: when a new password comes back cracked, push a
notification to the operator's phone via ntfy or a Discord webhook. The second slice-7 surface after
the LED, and the first that transmits — it observes `NewPassword` on the surface event bus (ADR-0021)
and POSTs inside the supervisor's STA window (ADR-0023). The payload names *which* network was cracked
(ESSID + BSSID) and never carries the plaintext PSK (ADR-0023 decision 4; operator's choice).

## Definition of Done

**Scenario A — a new password is enqueued (host).**
- **Given** a `WebhookNotifier` subscribed to a bus
- **When** a `NewPassword` fact is published
- **Then** it enqueues one pending notification carrying the ESSID and BSSID — and no password field
  exists to carry the PSK.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario B — non-crack facts are ignored (host).**
- **Given** the notifier
- **When** `DrainStarted`, `DrainCompleted`, `SyncCompleted`, or `FirstSyncSummary` is published
- **Then** nothing is enqueued (a fresh-manifest backlog raises no push — ADR-0019 decision #5).
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario C — a window flush sends and clears (host).**
- **Given** one or more pending notifications and a transport that accepts
- **When** `flushInWindow()` runs
- **Then** every pending notification is POSTed once and the queue is emptied.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario D — a failed send is retried next window (host).**
- **Given** a pending notification and a transport that reports failure
- **When** `flushInWindow()` runs, then a later `flushInWindow()` runs against a now-accepting transport
- **Then** the notification stays queued after the failure and is sent on the retry — no loss, no
  duplicate after success.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario E — overflow drops loud, never overruns (host).**
- **Given** the pending queue at capacity
- **When** another `NewPassword` arrives
- **Then** the overflow is counted (a non-zero dropped count) and no write occurs past the fixed buffer.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario F — ntfy rendering (host).**
- **Given** a notification and an ntfy target
- **When** it is rendered and posted
- **Then** the request carries a `Title` header and a plain-text body naming the ESSID and BSSID,
  `Content-Type` is unset, and the plaintext PSK appears nowhere in the output.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario G — Discord rendering (host).**
- **Given** a notification and a Discord target
- **When** it is rendered and posted
- **Then** the body is JSON (`{"content":"…"}`), `Content-Type` is `application/json`, an ESSID
  containing `"` or `\` is escaped so the JSON stays well-formed, and the PSK appears nowhere.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario H — target detection (host).**
- **Given** a configured URL
- **When** `detectWebhookTarget()` runs
- **Then** a `discord.com`/`discordapp.com` host maps to Discord and every other host (ntfy.sh, a
  self-hosted domain) maps to ntfy.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario I — provisioning is extended without gating boot (host).**
- **Given** a candidate webhook URL
- **When** `isUsableWebhookUrl()` runs, and separately `validateCredentials()` runs on the triad
- **Then** empty/non-`https://`/over-length → unusable and a well-formed `https://` URL → usable; and
  `validateCredentials()` returns the same result whether or not a `webhookUrl` is present (a bad
  webhook URL never sends a good triad to the portal).
- **Proof:** `npm run test:native` → `test/test_provisioning`.

**Scenario J — the supervisor flushes inside a live window (host).**
- **Given** a supervisor with a notifier set
- **When** a drain cycle runs with a station that comes up, and separately one that cannot associate
- **Then** `flushInWindow()` is called once for the live cycle (station up) and not at all for the
  offline cycle; with no notifier set the drain behaves exactly as slice-0018/0022.
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario K — on-air: a real crack fact reaches a real push service (device).**
- **Given** a provisioned webhook URL, real WiFi, and a wpa-sec key
- **When** the bench probe injects a synthetic `NewPassword` and forces an STA window
- **Then** the transport POSTs to the live ntfy/Discord endpoint and the service returns a 2xx (logged
  `[WEBHOOK] sent … code=2xx`), with the TLS handshake validated against the system root bundle.
- **Proof:** `npm run verify:webhook` (`scripts/0024-webhook-verify.mjs`); artifact
  `artifacts/0024-webhook.txt`.

## Design

Per ADR-0023. Components:

- **`net/window_notifier.h`** — the `WindowNotifier` seam (one `flushInWindow()`), the supervisor's
  abstract window-sharer contract; keeps `net` independent of `surface`.
- **`surface/webhook_notifier.{h,cpp}`** (pure, host-tested) — `WebhookNotification` (ESSID + BSSID,
  no PSK); `WebhookTarget` + `detectWebhookTarget()`; `WebhookRequest` (url, contentType, title,
  body); the `WebhookTransport` seam; `WebhookNotifier : EventSink, WindowNotifier` holding the
  fixed pending queue, the pure per-target rendering, and the send-and-keep flush.
- **`surface/webhook_transport_esp32.{h,cpp}`** (device-only) — `Esp32WebhookTransport`:
  `WiFiClientSecure` + `setCACertBundle(...)` + `HTTPClient` POST; logs `[WEBHOOK] sent/err`.
- **`net/upload_supervisor.{h,cpp}`** — `setNotifier(WindowNotifier&)` + one guarded
  `flushInWindow()` call in `runDrainCycle()` (after the sync, before tear-down).
- **`net/provisioning.{h,cpp}`** — `kMaxWebhookUrlLen`, `isUsableWebhookUrl()` (pure);
  `net/provisioning_store.{h,cpp}` — `webhookUrl` field + NVS key, through the existing seam.
- **`hunt_loop.cpp`** — construct the transport + notifier, subscribe + `setNotifier()` iff the URL is
  usable; **`webhook_probe.{h,cpp}`** + `main.cpp` dispatch — the bench verify probe.

## Verification

- **Scenarios A–I** (pure policy): `test/test_webhook_notifier/` (A–H) and `test/test_provisioning/`
  (I), native Unity lane (ADR-0004 lane 1), `npm run test:native`.
- **Scenario J**: `test/test_upload_supervisor/` — a `FakeWindowNotifier` asserts the in-window call;
  native lane.
- **Scenario K** (on-air): `scripts/0024-webhook-verify.mjs` (`npm run verify:webhook`), driving
  `env:cardputer_testhooks` with `SAPPER_TEST_WEBHOOK=1` and a provisioned URL. Fails on any
  `[FATAL]`/`[ERROR]`; success requires `[WEBHOOK] sent … code=2xx`. Artifact:
  `artifacts/0024-webhook.txt`.
- Board compile (ADR-0004 lane 2): `pio run -e cardputer` and `-e cardputer_testhooks`.

## As built

Shipped as designed (ADR-0023), with these deviations and additions worth recording:

- **The captive portal gained the field.** The Design listed the NVS/seam plumbing but not the portal
  UI; without it the webhook would be reachable only via the test-hooks seed, i.e. unusable on a
  shipped device. So `captive_portal.cpp` grew an optional `webhook` input (`type='url'`, maxlength
  160) and `handleSave()` rejects a non-blank, non-`https://` value loudly rather than saving-and-
  ignoring it. Backward-compatible: a blank field stores as disabled, so the slice-0007 provisioning
  verify (which submits only ssid/pass/key) is unaffected.
- **Measured flash cost corrected the ADR.** ADR-0023 decision 5 first claimed the CA bundle was
  "zero incremental flash" (it ships in `libmbedtls.a`). The board compile disproved that: the linker
  pulls an archive member only when referenced, and nothing referenced the bundle before this slice.
  Attaching it took the shipped image **38.3% → 40.5%** flash (+~73 KB: the ~67 KB bundle blob +
  webhook code), RAM steady at 35.3%. The ADR now records the measured cost (§4: cite measured data;
  §2: report evidence against one's own claim).
- **`setNotifier()` post-construction wiring**, not a ninth constructor argument (ADR-0023 decision 2),
  mirroring the existing `CaptureReadyRelay::setTarget` idiom in the same file.
- **`huntLoopWebhookSentCount()`** was added so the on-air probe can detect each send and re-arm,
  paralleling `huntLoopSyncCount()`; it reads the notifier's `sentCount()` (0 when push is disabled).
- **`Esp32WebhookTransport`** const-casts the body for `HTTPClient::POST` (which takes a non-const
  payload it does not modify) and never logs `request.url` (it carries the ntfy topic / Discord token).
  ntfy sends carry `Title` + `Priority: high` headers.
- **The no-PSK guarantee is structural**, not disciplinary: `WebhookNotification` has no password
  field, so no rendering path can leak the PSK even by mistake — two tests assert the PSK appears in
  neither the ntfy nor the Discord body.
- **Measured host lanes:** webhook_notifier 8, upload_supervisor 14 (incl. Scenario J), provisioning 21
  (incl. the five webhook-URL cases), and the migrated suites green (provisioning_store 5, led 9,
  event_bus 5, cracked_sync 7, sync_session 2). Both board envs compile. `pio` native is broken in
  this environment (UnknownPlatform), so the native lane was run via direct g++ compile, the
  established workaround.
- **Deferrals recorded** (LEDGER): a self-hosted ntfy behind a private CA is unsupported (bundle trusts
  public roots); the on-air proof is per-target (whichever of ntfy/Discord the operator runs); retry
  is window-paced with no per-item backoff and the notifier never requests its own window.
- **On-air Scenario K passed against a live Discord webhook** (`HTTP 204`), artifact
  `artifacts/0024-webhook.txt` (secret-free — the transport never logs the URL/token).
- **Adversarial review (blind, Sonnet 5) — two findings, both fixed here, no rejections:**
  - *MEDIUM* — a non-UTF-8 ESSID (802.11 SSIDs are arbitrary octets) produced a Discord JSON body Discord
    rejects with 400; the kept-for-retry entry would occupy a queue slot forever and, once
    `kMaxPendingNotifications` such entries accumulated, silently drop every new crack's alert. Fixed at
    the root: `sanitizeToUtf8()` cleans the ESSID to valid UTF-8 at enqueue (invalid bytes → `?`, valid
    international SSIDs preserved), for both targets. Regression:
    `test_invalid_utf8_essid_is_sanitized_before_send`.
  - *LOW* — `detectWebhookTarget` matched `discord.com` as a whole-URL substring, so a ntfy topic path
    like `/discord.com-alerts` misdetected as Discord. Fixed with a host-scoped match (`hostIs`);
    subdomains (`canary.discord.com`) still map to Discord, a mere suffix (`notdiscord.com`) does not.
    Regression added to `test_detect_target_from_url`. webhook_notifier suite now 9 tests.
  - The fixes are backward-compatible with the committed on-air artifact (the verify's ASCII ESSID and
    `discord.com` URL are unaffected), so no re-flash was required.
