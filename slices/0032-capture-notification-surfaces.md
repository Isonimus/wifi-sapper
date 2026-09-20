---
id: '0032'
title: "Capture-notification surfaces: a HandshakeCaptured bus fact the screen, LED and serial announce"
type: slice
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Goal

Close the gap where a captured-and-uploaded handshake raises no local signal (only the HUD's `up`
counter moved). Per ADR-0031: publish a cosmetic `AppEventType::HandshakeCaptured` from
`UploadSupervisor::onCaptureReady` after a successful enqueue, carrying an identity-only `CaptureFact`
(bssid + ssid, never the pcap frames — §4 #9/#17); have the screen raise a `CAPTURED` banner, the LED
give a brief capture flash, and the serial logger print `[CAPTURE] …`. The capture→queue data path
stays the direct `CaptureReadyObserver` seam (§4 #13); the bus fact is an additional best-effort
broadcast. The webhook is untouched (webhook-on-capture stays its own LEDGER slice).

## Definition of Done

**Scenario A — a successful enqueue publishes HandshakeCaptured (host).**
- **Given** an `UploadSupervisor` wired to an `EventBus` with a recording sink, and a capture whose
  `queue_.offer()` returns `Stored`
- **When** `onCaptureReady(handshake)` runs
- **Then** the sink receives exactly one `AppEvent` of type `HandshakeCaptured` whose `CaptureFact`
  carries the handshake's `bssid` and `ssid`, published after the enqueue (ADR-0031 #1/#3).
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario B — a failed enqueue publishes nothing (host).**
- **Given** the same supervisor, with the queue configured to return `StoreError` (and separately
  `NotUploadable`)
- **When** `onCaptureReady(handshake)` runs
- **Then** the recording sink receives no `HandshakeCaptured` — the fact never announces a capture the
  queue dropped (ADR-0031 #1).
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`.

**Scenario C — the payload is identity-only (host + compile-time).**
- **Given** a published `HandshakeCaptured` event
- **When** a sink reads `event.capture`
- **Then** it can read only `bssid` and `ssid` — `CaptureFact` has no frame/pcap member, so no surface
  can reach capture bytes through the bus (ADR-0031 #3; §4 #9/#17). The `ssid` copied to a surface
  equals the source; a hidden network's empty `ssid` is carried as empty.
- **Proof:** `npm run test:native` → `test/test_upload_supervisor` (payload contents);
  `test/test_event_bus` (factory sets `capture`, leaves the other payload pointers null).

**Scenario D — the screen raises a CAPTURED banner naming the network (host).**
- **Given** a `ScreenToastSurface` with a fake renderer, begun
- **When** a `HandshakeCaptured` for ssid "lab-ap" / a known BSSID arrives and `tick()` runs
- **Then** the rendered `ScreenView` has `toastActive == true`, `ToastKind::Captured`, and the
  banner ESSID/BSSID equal the captured network's (printable-filtered); the banner clears after its
  (shorter than CRACKED) hold (ADR-0031 #4).
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario E — a crack banner outranks a capture banner (host).**
- **Given** a `ScreenToastSurface` currently showing a CRACKED banner (a `NewPassword` was toasted)
- **When** a `HandshakeCaptured` arrives before the CRACKED hold elapses
- **Then** the CRACKED banner is not replaced — `ToastKind` stays `Cracked` for the remainder of its
  hold; a `HandshakeCaptured` arriving with no banner active does raise CAPTURED (ADR-0031 #4).
- **Proof:** `npm run test:native` → `test/test_screen_toast_surface`.

**Scenario F — the LED gives a brief capture flash, outranked by Recovered (host).**
- **Given** a `LedStatusSurface` with a fake driver, begun (heartbeat)
- **When** a `HandshakeCaptured` arrives and `tick()` runs
- **Then** the driver shows `LedStatus::Captured` for its (brief) hold, then returns to the
  heartbeat; and when a `NewPassword` flash is active, a concurrent `HandshakeCaptured` does not
  override the `Recovered` latch (render precedence Recovered > Captured, ADR-0031 #4).
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario J — a shipped-seam capture fires the announcement end-to-end on hardware (on-air, deterministic).**
- **Given** a `cardputer_testhooks` board running the hunt loop
- **When** the inject-capture serial command drives a capture through the real
  `CaptureReadyObserver` seam (§4 #2)
- **Then** the device logs a `[CAPTURE] essid=… bssid=…` line (no `[FATAL]`/`[ERROR]`), and the panel
  shows the CAPTURED banner — proving the bus dispatch, serial logger, and screen fire end-to-end on
  real hardware, deterministically rather than waiting for an environmental handshake.
- **Proof:** `npm run verify:capture-notify` → `scripts/0032-capture-notify-verify.mjs` (asserts the
  `[CAPTURE]` line; writes the serial log + a panel-photo prompt as the eyeball artifact).

## Design

- **Bus (`core/event_bus.h`)**: add `AppEventType::HandshakeCaptured`; add
  `struct CaptureFact { uint8_t bssid[6]; char ssid[33]; };` (a small POD, forward-declared like the
  other payloads); add `const CaptureFact* capture = nullptr;` and a
  `AppEvent::handshakeCaptured(const CaptureFact&)` factory. Pure, host-tested.
- **Producer (`net/upload_supervisor.cpp`)**: in `onCaptureReady`, on the `Stored`/`Replaced`/`Evicted`
  arms, build a stack-local `CaptureFact` from `handshake.bssid`/`handshake.ssid` and
  `bus_->publish(AppEvent::handshakeCaptured(fact))` — after the enqueue, held live across the
  synchronous publish. No publish on `NotUploadable`/`StoreError`.
- **Screen (`surface/screen_view.h`, `screen_toast_surface.*`)**: add `enum class ToastKind { Cracked,
  Captured }` and a `ToastKind toastKind` field to `ScreenView` (compared in `operator==`). The
  surface keeps a pending kind; `NewPassword` arms `Cracked` (always), `HandshakeCaptured` arms
  `Captured` only when no banner is currently pending/active as `Cracked`. A `kCapturedHoldMs` shorter
  than `kToastHoldMs`. The device renderer draws the label from `toastKind`.
- **LED (`surface/led_driver.h`, `led_status_surface.*`)**: add `LedStatus::Captured`; the surface
  arms a `capturedPending_/Active_/UntilMs_` latch (`kCapturedHoldMs`, brief) on `HandshakeCaptured`;
  `render()` precedence becomes Recovered > Captured > Fault > heartbeat. The device driver maps
  `Captured` to a distinct colour.
- **Serial (`hunt_loop.cpp` `SerialEventLogger`)**: add the `HandshakeCaptured` case →
  `[CAPTURE] essid=… bssid=…` (no PSK; a capture has none).
- **Webhook**: unchanged — `WebhookNotifier::onAppEvent` already ignores everything but `NewPassword`.
- **`-Wswitch`**: adding the enum value makes `-Wall -Wextra` fail every `switch (event.type)` lacking
  the case, so the screen, LED and serial switches each gain a real case by compiler mandate.

## Verification

- **Host (`npm run test:native`)** proves the policy — Scenarios A–F:
  - `test/test_upload_supervisor` — publish-on-enqueue-success, no-publish-on-failure, payload
    contents (A, B, C).
  - `test/test_event_bus` — the `handshakeCaptured` factory sets `capture` and leaves the other
    payload pointers null (C).
  - `test/test_screen_toast_surface` — CAPTURED banner contents + hold, crack-outranks-capture (D, E).
  - `test/test_led_status_surface` — brief capture flash + Recovered-outranks-Captured precedence (F).
  Built with `g++ -Wall -Wextra` (the `pio` native lane is broken here); a warning is a failure, and
  the new enum case in every switch is what keeps the build clean.
- **On-air (`npm run verify:capture-notify` → `scripts/0032-capture-notify-verify.mjs`)** proves
  Scenario J: drives `cardputer_testhooks`, injects a capture through the real seam, asserts the
  `[CAPTURE]` line appears with no fatal, and writes `artifacts/0032-capture-notify.txt` plus a
  panel-photo prompt for the CAPTURED banner (the eyeball half). Runs on an attached board, never in
  cloud CI (§4 #5).

## As built

Shipped as designed. `AppEventType::HandshakeCaptured` carries an identity-only `CaptureFact`
(`bssid` + `ssid`), defined in `core/event_bus.h` — never `CapturedHandshake` — so §4 #9's pcap
quarantine holds structurally on the bus (#17). `UploadSupervisor::onCaptureReady` publishes it after a
successful enqueue (`Stored`/`Replaced`/`Evicted`), never on failure; `queue_.offer()` is untouched
(#13). The screen raises a `CAPTURED` banner via a new `ScreenView::ToastKind` (crack outranks capture,
`kCapturedHoldMs` 2500 < the crack's 6000); the LED gives a brief `LedStatus::Captured` flash
(`kCapturedHoldMs` 800, precedence Recovered > Captured); the `SerialEventLogger` prints
`[CAPTURE] essid=… bssid=…`. The webhook is unchanged. Adding the enum drove a real case into every
`switch (event.type)` under `-Wall -Wextra -Wswitch`.

Proven: host **46 combined cases across the four affected suites, 0 failures** (`test_upload_supervisor`
17, `test_event_bus` 6, `test_screen_toast_surface` 12, `test_led_status_surface` 11), plus the full
25-dir suite green — `g++ -Wall -Wextra`, no warnings (`pio` native is broken here). Both boards
compile clean: shipped `cardputer` Flash **40.7% (1,360,115 bytes, +636 over slice-0030)**;
`cardputer_testhooks` builds the new capture probe. On-air **Scenario J PASS** on a real Cardputer ADV
via `verify:capture-notify` (SHIPPED-seam capture through the `cardputer_testhooks` probe, no network):
a repeating `[CAPTURE] essid='SAPPER' bssid=02:53:41:50:50:52` line, and a rendered panel
(`artifacts/0032-capture-notify.png`, 39.6% of pixels off the dominant colour — the banner drew). No
PSK or pcap byte on any surface, serial line, or the artifact.

A blind adversarial pass (Sonnet 5) found no correctness, invariant, or security issue — it
independently recompiled the four suites and confirmed the new positive tests fail before the
`publishCaptured()` call and pass after (real regressions). Its two findings, both non-blocking: (1) a
capture whose bytes DID land but whose reconcile-stage `store_.remove()` failed reports `StoreError`,
which suppresses the notification for a capture that was not dropped — a pre-existing coarse
`OfferResult` granularity (ADR-0017), cosmetic-only (#13), rare, recorded as a LEDGER defect rather
than coarsened here; (2) the CAPTURED banner title renders `kBlue` while the LED capture hue is cyan —
the misleading "cyan title" comment was corrected (the two surfaces pick their own distinct-from-crack
hue). Scenario K was not a separate item here — the probe injects deterministically, so J is the gate.
