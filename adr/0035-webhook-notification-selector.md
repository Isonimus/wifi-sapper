---
id: '0035'
title: "Webhook-on-capture + a per-type push selector: the enable-set gates the transmitting webhook only"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

The push webhook (ADR-0023) fires on exactly one fact: `NewPassword`. Two gaps have surfaced since:

- **No capture push.** The one event an operator most wants off-device during an active hunt — "a
  handshake just landed" — never leaves the device. The capture *fact* now exists on the bus
  (`AppEventType::HandshakeCaptured`, ADR-0031, identity-only `CaptureFact`), so the webhook *can* now
  react to it; it simply does not. slice-0032 solved the "went unnoticed" pain locally (screen banner,
  LED flash, `[CAPTURE]` serial line), but a headless unit in a factory has no local glance.
- **No control over push volume.** The webhook is all-or-nothing: provision a URL and every crack
  pushes. Adding capture push makes this acute — an endless autohunt over a room full of APs, each
  re-emitting `HandshakeCaptured` on every renegotiation, would turn ntfy/Discord into noise. And some
  operators want crack alerts but not capture spam, or want to hear that the hourly cracked-results sync
  is *failing* while the device is otherwise associated (wpa-sec unreachable/erroring) without either.
  (A *total* connectivity loss cannot be pushed at all — with no STA window there is nothing to send
  through, §4 #14 — so that case stays the local LED/screen's job; see the Consequences blind-spot note.)

The design forces:

- **§4 invariant #14** already fixes the transmit boundary: a webhook transmits only inside a
  supervisor-owned STA window via the `WindowNotifier` seam, never blocking IO in `onAppEvent`, and its
  payload carries no plaintext PSK (the `WebhookNotification` type has no password field). Anything
  added here must stay inside that boundary.
- **§4 invariant #17** made the capture bus fact structurally frame-free: `CaptureFact` is bssid + ssid,
  no pcap member. A capture push copying from it is therefore *structurally* incapable of carrying
  capture bytes — the #9 quarantine holds by construction on the push path too.
- **The enable-set must live where a rule that governs the codebase lives** — provisioned state in NVS
  through the #6 seam, exactly like the ADR-0029 `deauthEnabled` arm toggle — not a compile flag or a
  serial command (§4 invariant #7 keeps provisioning off the serial vocabulary).

A scope question the operator settled before this ADR: **what does the enable-set gate?** The choice was
*the transmitting webhook only* — not the local LED/screen/serial. The reasoning: off-device push is the
only channel with real noise, cost, and privacy stakes; the local surfaces are cheap glances an operator
rarely wants blinded, and the "went unnoticed" pain they answer is already solved (slice-0032). Gating
every surface uniformly would add branch logic to each cosmetic renderer for no benefit and a larger
blast radius. So the selector is a *push* policy, not a global one.

A second question the operator settled: **capture push volume** — *one push per BSSID per run*, not one
per fact. A network that renegotiates repeatedly (the normal case under an endless hunt, more so when
deauth is armed) must not push repeatedly; the first capture of each BSSID is the news.

## Decision

1. **A pure `WebhookNotifyPolicy` the notifier consults, sourced from provisioned NVS state.**
   `WebhookNotifyPolicy { bool onCaptured; bool onCracked; bool onSyncError; }` (in
   `webhook_notifier.h`) is passed to the `WebhookNotifier` constructor. `ProvisioningRecord` gains the
   three `bool`s (`notifyCaptured`, `notifyCracked`, `notifySyncError`); `hunt_loop` maps them into the
   policy. The notifier stays pure and decoupled from the device provisioning header — it is handed a
   policy, not a record — so all gating logic is host-tested against the fake transport (ADR-0004 lane 1).

2. **The webhook reacts to three fact types, each gated by the policy; local surfaces are never gated.**
   In `onAppEvent`:
   - `NewPassword` → if `onCracked`, enqueue a `Cracked` notification (the pre-0035 behaviour).
   - `HandshakeCaptured` → if `onCaptured` **and** this BSSID has not been pushed this run, enqueue a
     `Captured` notification (essid + bssid, from the frame-free `CaptureFact`) and record the BSSID.
   - `SyncCompleted` → if `onSyncError`, **edge-triggered**: on `!ok` while not already in a failing
     run, enqueue a `SyncError` notification and latch "failing"; on `ok`, clear the latch. One push per
     outage onset, not one per failed window — a persistent outage fails every STA window, and pushing
     each would spam (and each push itself needs a window that is not opening).

   The LED, screen, and serial surfaces continue to render every fact unconditionally. The enable-set is
   a property of the *transmitting* surface alone.

3. **Capture de-duplication: one push per BSSID for the device's run.** The notifier holds a fixed
   `seenBssids_` set (no heap). A BSSID already in the set is not re-enqueued. On overflow the oldest
   entry is evicted (a ring), so a very large environment recycles — a re-push of a long-since-seen
   network is acceptable; missing a genuinely new one is not — and the eviction count is exposed for
   fail-loud accounting (§3). The bound is architecture-derived (kMaxSeenBssids over a plausible
   authorized-site AP count), not a measurement; sizing against a real site is a LEDGER item.

4. **Per-kind rendering, still identity-only / secret-free.** Each queued `WebhookNotification` carries a
   `WebhookKind` (`Captured` / `Cracked` / `SyncError`); `send()` renders the matching title + body for
   both ntfy and Discord. `Captured` → "handshake captured" with essid/bssid; `Cracked` → the
   "password recovered - read on device" body (the phrase is now unified across targets, so the Discord
   body gains the "- read on device" suffix ntfy already had — a cosmetic change, no field/PSK impact);
   `SyncError` → a static "cracked-sync failing" line with **no** essid,
   bssid, or secret. No kind has a password field — §4 #14 holds by construction across all three, and
   the ESSID is still `sanitizeToUtf8`-cleaned at enqueue so a hostile SSID cannot wedge the retry queue.
   All three ride the one pending queue and flush together in the STA window (#14 unchanged).

5. **NVS migration + portal defaults, chosen to preserve behaviour and avoid surprise.** The NVS read
   (`loadProvisioning`) defaults an *absent* key via `getBool(key, default)`: `notifyCracked` **true**
   (a device provisioned before this slice keeps its ADR-0023 crack push), `notifyCaptured` and
   `notifySyncError` **false** (new and high-volume — opt-in, never a surprise flood after a firmware
   update). The captive-portal form ships the "cracked" checkbox `checked` and the other two unchecked,
   so a fresh provision matches the same defaults. The whole enable-set is moot unless a usable webhook
   URL is also provisioned (no URL → push disabled entirely, as today). The portal form's known
   statelessness (it re-enters all fields on re-save — a LEDGER defect) applies to these boxes exactly as
   to `deauthEnabled`; it is not widened or fixed here.

6. **On-air proof reuses the proven transport.** The new logic (gating, dedup, edge-trigger, per-kind
   render) is pure and host-tested. The live HTTPS POST path is unchanged from slice-0024, so the on-air
   scenario is a *sibling* of the slice-0024 webhook probe (leaving that crack verify untouched and
   independently runnable): a new `SAPPER_TEST_WEBHOOK_CAPTURE` probe that forces `onCaptured` on in RAM
   (test-hooks-only, §4 #4) so the proof does not depend on the provisioned box, injects a synthetic
   `HandshakeCaptured` through a `huntLoopInjectCaptureAlert()` hook (the same bus publish seam the
   supervisor uses, §4 #2), forces a due sync to open an STA window, and waits for the transport's send
   count to rise. It **re-drives** on each send — re-injecting a *distinct* capture (the hook increments
   the BSSID so the one-per-BSSID dedup does not suppress the re-injection) and re-arming the sync — so a
   fresh POST recurs each cycle; the reference board's native USB-CDC can drop serial delivery during a
   WiFi window, so a one-shot first send is not reliably observed, whereas a recurring POST lets a
   late/reconnecting monitor witness a `[WEBHOOK] sent code=` line (the same reason the slice-0024 crack
   probe re-drives). Per-target coverage stays the slice-0024 deferral (whichever of ntfy/Discord the
   operator provisions is the one exercised).

## Consequences

- `WebhookNotifier` gains a `WebhookNotifyPolicy` ctor arg, a `WebhookKind` per notification, the
  `seenBssids_` dedup set with eviction/overflow accounting, and the `syncFailing_` edge latch;
  `onAppEvent` switches on three fact types instead of one, and `send()` renders three kinds.
  `ProvisioningRecord` gains three `bool`s, written by `persistProvisioning` and read (with the
  behaviour-preserving defaults) by `loadProvisioning` — the #6 seam, no new NVS access site. The captive
  portal form gains three checkboxes and `handleSave` reads them like the deauth box.
- New §4 invariant **#19** records that off-device push is gated per-notification-type by the operator's
  provisioned enable-set, consulted by the transmitting webhook **only** (local surfaces are never gated
  by it), and that every pushed payload stays identity-only / secret-free across all three kinds
  (extends #14; builds on the frame-free capture fact of #17). Added in this ADR's commit (same-commit
  rule).
- `hunt_loop` gains a `SAPPER_TEST_HOOKS`-only `huntLoopInjectCaptureAlert()` (mirrors
  `huntLoopInjectCrackedAlert`); a new `webhook_capture_probe` (gated `SAPPER_TEST_WEBHOOK_CAPTURE`)
  drives it; the slice-0024 webhook probe and `scripts/0024-webhook-verify.mjs` are left intact so the
  crack-push proof stays stable — the capture-push proof is a new
  `scripts/0036-webhook-capture-verify.mjs` wired into `package.json` (R11). The shipped binary carries
  neither probe (#4).
- The seen-set bound and the portal-statelessness interaction are LEDGER items, not silent choices.
- ADR-0023 is extended, not superseded: the transmit boundary (#14), the enqueue-not-transmit split, and
  the target detection are unchanged; the webhook simply reacts to more facts, each gated.
- **Sync-error blind spot (recorded, not fixed).** `SyncCompleted` is published only when a drain window
  actually opened (`bringUpStation()` succeeded), so the sync-error push covers "associated, but the
  wpa-sec sync failed", not a total off-air outage. This is not a fixable gap for the *push* channel: by
  #14 a webhook can only transmit inside an STA window, so a device that cannot associate cannot push
  anything — the local LED/screen carry that case. A LEDGER `[deferred]` records it (a
  store-and-forward-on-reassociate could close it if the field ever needs it).
