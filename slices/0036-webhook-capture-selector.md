---
id: '0036'
title: "Webhook-on-capture + per-type push selector: fire on captures too, each notification type gated by a provisioned enable-set"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Make the push webhook (ADR-0023) fire on a fresh capture, not only a crack, and let the operator choose
per notification type what gets pushed. Per ADR-0035: add a pure `WebhookNotifyPolicy`
(`onCaptured`/`onCracked`/`onSyncError`) the `WebhookNotifier` consults; react in `onAppEvent` to
`HandshakeCaptured` (identity-only, de-duplicated one-per-BSSID-per-run) and `SyncCompleted` failures
(edge-triggered) as well as `NewPassword`, each gated by the policy; render a per-kind body; source the
enable-set from three NVS-backed `ProvisioningRecord` bools set by captive-portal checkboxes (the
ADR-0029 `deauthEnabled` pattern). The enable-set gates the transmitting webhook only — the LED, screen,
and serial surfaces stay always-on. No pushed payload carries a PSK or a pcap byte (§4 #14/#17/#19).

## Definition of Done

**Scenario A — a capture pushes when armed, is silent when not (host).**
- **Given** a `WebhookNotifier` over a fake transport, policy `{onCaptured:true}`
- **When** a `HandshakeCaptured` fact (essid "lab-ap"/a BSSID) is dispatched and a window flushes
- **Then** exactly one POST is made, its body names the ESSID and BSSID and says "captured" (no PSK
  field exists); with `{onCaptured:false}` no POST is made for the same fact.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario B — one push per BSSID per run (host).**
- **Given** policy `{onCaptured:true}`
- **When** three `HandshakeCaptured` facts arrive — BSSID X, BSSID X again, BSSID Y — across one or more
  flushes
- **Then** exactly two POSTs are made (X once, Y once); the second X is de-duplicated, and
  `droppedCount()`/the dedup accounting reflects no queue overflow.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario C — the dedup set evicts oldest on overflow (host).**
- **Given** policy `{onCaptured:true}` and more distinct BSSIDs than `kMaxSeenBssids`
- **When** a BSSID evicted from the seen-set is captured again
- **Then** it pushes again (eviction is by oldest, so recycling re-pushes rather than being lost), and
  the eviction count is exposed (fail-loud accounting, §3) — never a silent miss of a new BSSID.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario D — cracked push is gated, default preserves ADR-0023 (host).**
- **Given** a `NewPassword` fact
- **When** policy `{onCracked:true}` vs `{onCracked:false}`
- **Then** it pushes only when `onCracked`; the body names the recovered network ("password recovered")
  with no PSK. (Regression: the *gating* pre-0035 behaviour is exactly `onCracked:true`. The rendered
  phrase is now unified across targets — "password recovered - read on device" — so the Discord body
  gains the "- read on device" suffix ntfy already had; a cosmetic wording change, no PSK, no field
  change.)
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario E — sync-error is edge-triggered (host).**
- **Given** policy `{onSyncError:true}`
- **When** `SyncCompleted{ok:false}` arrives, then `{ok:false}` again, then `{ok:true}`, then
  `{ok:false}` again — with a flush after each
- **Then** POSTs happen on the first failure and on the fourth event only (the re-onset after a success)
  — two pushes, not three; the repeated failure and the success raise none. The sync-error body carries
  no essid, bssid, or secret.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario F — per-kind rendering is well-formed for both targets (host).**
- **Given** a Discord URL and, separately, an ntfy URL
- **When** each of a Captured / Cracked / SyncError notification is rendered
- **Then** the Discord body is valid JSON (`{"content":"…"}`, ESSID JSON-escaped) and the ntfy body is
  the plain-text form with the right Title header; each kind's text is distinct; none contains a
  password field.
- **Proof:** `npm run test:native` → `test/test_webhook_notifier`.

**Scenario G — the selector round-trips through NVS with behaviour-preserving defaults (host).**
- **Given** the store (over the fake `<Preferences.h>`): (a) a record with the three flags explicitly
  set is persisted then loaded, and (b) a legacy triad with the notify keys *absent* is loaded
- **When** `loadProvisioning` reads each back
- **Then** (a) the three flags round-trip (an explicit `notifyCracked=false` survives, not overridden by
  the default); and (b) an absent-key read yields `notifyCracked:true`, `notifyCaptured:false`,
  `notifySyncError:false` (ADR-0035 decision 5) — an upgraded device keeps its ADR-0023 crack push and
  gains no surprise flood. The `WebhookNotifyPolicy` struct default is asserted to match, so an omitted
  policy is the same safe set.
- **Proof:** `npm run test:native` → `test/test_provisioning_store` (`test_notify_selector_roundtrips`,
  `test_notify_absent_keys_load_with_migration_defaults`) + `test/test_webhook_notifier`
  (`test_default_policy_is_cracked_only`).

**Scenario H — a capture reaches the endpoint on real hardware (on-air, operator-provisioned).**
- **Given** a `cardputer_testhooks` board with a real network + wpa-sec key + usable webhook URL
  provisioned, running the `webhook_capture_probe` (`SAPPER_TEST_WEBHOOK_CAPTURE`, capture-push forced
  on, test-hooks-only)
- **When** the probe injects a synthetic `HandshakeCaptured` on the bus and an STA window opens
- **Then** the device logs a rising `sent` count and the configured ntfy/Discord endpoint receives a
  "captured" notification (essid/bssid, no PSK) — the capture-push path, end to end, on hardware.
- **Proof:** `npm run verify:webhook-capture` → `scripts/0036-webhook-capture-verify.mjs` (asserts a
  POST succeeded, no `[FATAL]`/`[ERROR]`; per-target coverage is the slice-0024 deferral).

## Design

- **Policy (`surface/webhook_notifier.h`)**: `struct WebhookNotifyPolicy { bool onCaptured=false;
  bool onCracked=true; bool onSyncError=false; }` (the struct defaults mirror the NVS migration
  defaults, so a default-constructed policy is the safe "cracked only" set). `WebhookNotifier` takes it
  as a ctor arg after the URL/transport.
- **Kind (`webhook_notifier.h`)**: `enum class WebhookKind : uint8_t { Captured, Cracked, SyncError };`
  stored on each `WebhookNotification`. `WebhookNotification` keeps its essid/bssid (both empty for a
  SyncError); still no password field.
- **Reaction (`webhook_notifier.cpp`)**: `onAppEvent` switches on the three types, gated by the policy;
  `HandshakeCaptured` consults/updates the dedup set; `SyncCompleted` consults/updates the
  `syncFailing_` latch. `send()` renders per kind for ntfy and Discord, reusing the existing
  `appendJsonEscaped`/`sanitizeToUtf8` helpers.
- **Dedup (`webhook_notifier.cpp`)**: `uint8_t seenBssids_[kMaxSeenBssids][6]`, a count, and a
  ring-eviction index; `droppedCount_`-style `evictedCount_` for accounting. Linear scan (N small, no
  heap).
- **Provisioning (`net/provisioning_store.h`/`.cpp`)**: `ProvisioningRecord` gains `notifyCaptured`,
  `notifyCracked`, `notifySyncError`; `persistProvisioning` writes them, `loadProvisioning` reads them
  with `getBool(key, default)` for the migration defaults. The `WebhookNotifyPolicy` struct defaults
  mirror those NVS read-defaults, so the behaviour-preserving default is host-tested directly.
- **Portal (`net/captive_portal.cpp`)**: three checkboxes replace the single "push on new password"
  notice — "cracked" `checked`, "captured"/"sync-error" unchecked; `handleSave` sets each from
  `m_http.arg("nX") == "on"`. Only meaningful when a webhook URL is set.
- **Wiring (`hunt_loop.cpp`)**: map `g_creds.notify*` into a `WebhookNotifyPolicy` and pass it to the
  `WebhookNotifier` ctor; nothing else in the surface graph changes.
- **Test stimulus (`hunt_loop.cpp`, `SAPPER_TEST_HOOKS`)**: `huntLoopInjectCaptureAlert()` publishes a
  synthetic `HandshakeCaptured` on `g_bus` (the supervisor's own publish seam, §4 #2), mirroring
  `huntLoopInjectCrackedAlert`. A sibling `webhook_capture_probe` (gated `SAPPER_TEST_WEBHOOK_CAPTURE`,
  the slice-0024 webhook probe left intact) forces `creds.notifyCaptured` on in RAM and drives it, so the
  capture→POST proof does not depend on the provisioned box. It re-drives on each send with a *distinct*
  BSSID (the inject hook increments it, defeating the one-per-BSSID dedup) so a fresh POST recurs each
  cycle — the reference board's native USB-CDC can drop serial delivery during a WiFi window, so a
  recurring POST, not a one-shot, is what a late/reconnecting monitor can witness (as the crack probe does).

## Verification

- **Host (`npm run test:native`)** proves Scenarios A–G:
  - `test/test_webhook_notifier` — capture push + gating, one-per-BSSID dedup + eviction, cracked
    gating, sync-error edge-trigger, per-kind ntfy/Discord rendering (A–F), and the `WebhookNotifyPolicy`
    default (G), against the fake transport.
  - `test/test_provisioning_store` — the selector round-trips through the NVS seam and applies the
    behaviour-preserving absent-key defaults (G), over the fake `<Preferences.h>`.
  Built with `g++ -Wall -Wextra`; a warning is a failure.
- **On-air (`npm run verify:webhook-capture` → `scripts/0036-webhook-capture-verify.mjs`)** proves
  Scenario H: drives the webhook probe with capture-push forced on, asserts a rising `sent` count and no
  fault. Attached board, never cloud CI (§4 #5); per-target is the slice-0024 deferral.

## As built

Shipped as designed. `WebhookNotifyPolicy` (`onCaptured`/`onCracked`/`onSyncError`, struct defaults
`{false,true,false}`) is a ctor arg on `WebhookNotifier` (defaulted, so an omitted policy is the safe
"cracked only" set); `onAppEvent` switches on the three fact types, each policy-gated, and enqueues a
`WebhookKind`-tagged `WebhookNotification`. Captures de-duplicate one-per-BSSID-per-run through a fixed
`seenBssids_` ring (`kMaxSeenBssids=128`, oldest-evicted, `evictedCount()` accounted); `isSeen` is checked
before `enqueue` and `recordSeen` only after it queues, so an overflow-dropped capture is never silently
marked seen. Sync-error is edge-triggered via `syncFailing_`. `send()` renders per kind for ntfy/Discord
(the "Cracked" phrase is now unified across targets — the Discord body gains "- read on device"; cosmetic,
no field/PSK change). `ProvisioningRecord` gained `notifyCaptured`/`notifyCracked`/`notifySyncError`,
written by `persistProvisioning` and read by `loadProvisioning` with `getBool(key, default)` migration
defaults (cracked true, others false); three captive-portal checkboxes ("cracked" `checked`) set them;
`hunt_loop` maps the record into the policy. The enable-set gates the transmitting webhook only (§4 #19);
LED/screen/serial are unchanged.

**One deviation, forced by the on-air run (the verify earned its keep):** the first `verify:webhook-capture`
failed (`no 2xx send in 90000ms`) even though the capture POST *did* reach the endpoint. Root cause: the
probe injected a single capture, and the reference board's native USB-CDC drops serial delivery during a
WiFi window, so the transport's one-shot `[WEBHOOK] sent code=` line was lost with no second send to catch
— unlike the slice-0024 crack probe, which re-drives. Fixed by re-driving: `huntLoopInjectCaptureAlert()`
increments the BSSID per call (so the one-per-BSSID dedup does not suppress the re-injection) and the probe
re-injects + re-arms the sync on each send, so a fresh POST recurs each cycle. (A separate operator-side
false start — a stale `SAPPER_TEST_HUNT_HUD` env var made an earlier probe seize the board ahead of this
one — was diagnosed from the empty artifact, not a code change.)

Proven: host **25 dirs green** — `test_webhook_notifier` **17** (per-type gating, one-per-BSSID dedup +
oldest-eviction, cracked gating, sync-error edge-trigger, per-kind secret-free ntfy/Discord render, default
policy), `test_provisioning_store` **9** (added the selector NVS round-trip and the absent-key
migration-default test, the latter proven fail-before/pass-after by flipping the `getBool` default) —
`g++ -Wall -Wextra`, no warnings (`pio` native broken here). Both boards compile clean: shipped `cardputer`
Flash **40.8% (1,362,959 bytes)**; `cardputer_testhooks` builds the sibling probe (1,363,611). On-air
**Scenario H PASS** on a real Cardputer ADV via `verify:webhook-capture`: `[WEBHOOK-CAPTURE] pushed count=1`
+ `[WEBHOOK] sent code=204 bytes=95` (Discord's 2xx), the "captured" notification confirmed arrived at the
live endpoint carrying ESSID/BSSID and **no PSK or pcap byte** on any surface, serial line, or the artifact.

A blind adversarial pass (Sonnet 5) found no BLOCKER, crash, secret leak, or invariant violation, and
positively cleared the no-PSK/no-pcap guarantee across all three kinds, the dedup ring/eviction ordering,
the sync-error latch, and the NVS all-or-nothing write path. Its findings were addressed: (HIGH) the stale
LEDGER item for this shipped feature was deleted, and the false `verified_by: test/test_provisioning`
citation was corrected to `test/test_provisioning_store` with the genuinely-missing NVS migration-default
test added; (MEDIUM) the sync-error README/ADR wording was corrected — it fires on an *associated* sync
failure, not a total off-air outage (which by §4 #14 can push nothing at all), and that inherent blind spot
is now a LEDGER `[deferred]`; (LOW) the slice/ADR wording was corrected to acknowledge the unified Discord
"Cracked" phrase.
