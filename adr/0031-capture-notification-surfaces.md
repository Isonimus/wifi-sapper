---
id: '0031'
title: "Capture-notification surfaces: an identity-only HandshakeCaptured bus fact"
type: architecture
status: accepted
date: 2026-09-20
supersedes: []
superseded_by: []
---

## Context

An operator watching a live board captured and uploaded a handshake and saw *nothing* announce it:
the only visible change was the HUD's cumulative `up` counter ticking +1. The cause is architectural,
not a missed glance. The surface event bus (ADR-0021) carries five facts — `DrainStarted`,
`DrainCompleted`, `SyncCompleted`, `NewPassword`, `FirstSyncSummary` — and **none of them is "a
handshake was just captured."** The screen toast (ADR-0025) and the LED flash (ADR-0021) both arm
only on `NewPassword`, which is a *crack* returning from wpa-sec hours or days later, not the capture
moment. So the single most frequent, most operator-relevant event the appliance produces — it just
got a handshake — reaches no local surface at all.

The capture *data* already has a home: the handshake flows engine → `CaptureReadyObserver` →
`UploadSupervisor` → the upload queue, a must-deliver single-consumer pipe that §4 invariant #13
deliberately keeps *off* the bus (a dropped fact is cosmetic; a dropped capture is a lost handshake).
That boundary is correct and stays. What is missing is the *cosmetic* counterpart: a best-effort
"it happened" broadcast a surface may observe or ignore — exactly what the bus exists to carry
(ADR-0021). ADR-0021 anticipated this split; #13 names it in as many words ("a dropped fact is
cosmetic"). This ADR adds the fact.

One hazard shapes the payload. The bus's existing rich payloads are borrowed pointers to
producer-owned structs (`DrainOutcome`, `CrackedResult`); `NewPassword` hands the whole
`CrackedResult` — *including its `password` field* — and relies on each surface to copy only the
non-secret fields (ADR-0025 decision 4; ADR-0023's no-PSK guarantee). A capture is stricter: the
`CapturedHandshake` struct carries the raw 802.11/EAPOL **pcap frames**, and §4 invariant #9
quarantines capture bytes to the `CaptureSink` seam — "no module opens or writes a capture file
directly." Borrowing a `const CapturedHandshake*` onto the bus would hand every surface a pointer to
those frames, inviting a future surface to grow a second capture-bytes path that #9 forbids. Trusting
each surface to look away from the frames (the `NewPassword` model) is weaker than the invariant
deserves.

## Decision

1. **A new cosmetic fact `AppEventType::HandshakeCaptured`**, published by `UploadSupervisor::onCaptureReady`
   immediately **after** a successful enqueue — the `Stored` / `Replaced` / `Evicted` outcomes, never
   `NotUploadable` / `StoreError`. The supervisor already owns the bus and publishes the drain facts,
   so the producer does not move and the `HuntEngine` stays pure and bus-free. Publishing *after* the
   `queue_.offer()` means the fact can never announce a capture the queue actually dropped.

2. **The capture→queue data path is untouched.** `queue_.offer()` remains the must-deliver
   `CaptureReadyObserver` seam of §4 #13; the `publish()` is an *additional* best-effort broadcast on
   the same call, after the data has landed. A surface that misses the fact loses a banner, never a
   handshake.

3. **The payload is an identity-only value, `CaptureFact { uint8_t bssid[6]; char ssid[33]; }`,
   borrowed by pointer** — *not* the `CapturedHandshake`. The supervisor fills a stack-local
   `CaptureFact` from the handshake's `bssid`/`ssid` and holds it live across the synchronous
   `publish()`, exactly as the other producers hold their payloads. This makes #9's pcap quarantine
   **structural on the bus**: a surface literally cannot reach a capture frame through the bus, so no
   future surface can grow a second capture-bytes path. It is deliberately stricter than
   `NewPassword`'s hand-the-whole-struct model, because pcap bytes are precisely what #9 isolates and
   a structural guarantee beats a per-surface convention.

4. **Three surfaces react; the switch compiler enforces that each decides.** Adding the enum value
   makes `-Wall -Wextra`'s `-Wswitch` fail every `switch (event.type)` that does not handle it, so
   each surface consciously opts in or out:
   - **Screen** (`ScreenToastSurface`): a `CAPTURED <ssid|bssid>` banner reusing the existing
     pending→active→hold machinery. A `ToastKind` field on `ScreenView` lets the renderer label
     CAPTURED vs CRACKED. The banner hold is shorter than the crack banner's (captures are frequent),
     and a currently-showing CRACKED banner is **not** replaced by a capture — the crack is the
     rarer, bigger news and keeps the slot; a later capture or crack still (re)arms.
   - **LED** (`LedStatusSurface`): a brief `LedStatus::Captured` flash, held shorter than the
     `Recovered` latch, with render precedence Recovered > Captured > Fault > heartbeat. On a
     screen-less unit the LED is the *only* local surface, so headless-first (the project's identity)
     wants a capture signal here; keeping the flash brief stops continuous hunting from strobing the
     indicator or drowning the heartbeat and the rarer recovered flash.
   - **Serial** (`SerialEventLogger`): a `[CAPTURE] essid=… bssid=…` line — the headless log record
     and the on-air verify's machine-checkable anchor. It carries no PSK (a capture has none).

5. **The webhook surface is unchanged**: `WebhookNotifier::onAppEvent` still early-returns on any
   fact but `NewPassword`, so it ignores `HandshakeCaptured`. A *transmitting* capture notification
   (webhook-on-capture) is a separate, larger unit — it needs the supervisor-owned STA window
   (§4 #14) and a per-notification-type enable set — and stays its own LEDGER slice.

6. **Proof splits by determinism.** The *policy* is fully host-testable and is: the supervisor
   publishes `HandshakeCaptured` exactly on a successful enqueue and not on a failed one; the toast,
   LED, and serial reactions produce the right view/status/line against their fakes, including the
   crack-beats-capture banner precedence. The *on-air* half — that a real board's bus dispatch,
   panel, and LED actually fire end-to-end — is proven **deterministically**, not by waiting for an
   environmental handshake: the `cardputer_testhooks` inject-capture serial command drives the same
   `CaptureReadyObserver` seam a real capture uses (§4 #2), so the verify injects a capture and
   asserts the `[CAPTURE]` line appears, with the panel banner as the committed eyeball artifact.

## Consequences

- The bus gains a sixth `AppEventType` and a new `CaptureFact` payload type; `AppEvent` gains one
  borrowed `const CaptureFact*` member and a `handshakeCaptured()` factory. The struct stays tiny and
  heap-free — the payload is borrowed, not inlined (ADR-0021).
- `ScreenView` gains a `ToastKind` enum field so one banner slot can render two labels; its
  `operator==` compares it, keeping render-on-change correct.
- `LedStatus` gains a `Captured` value; a device `LedDriver` renders it as a distinct colour (an RGB
  LED) or a blink the existing hardware already supports.
- A new §4 invariant (**#17**) records that capture *facts* may ride the bus only as this identity-only
  payload, so the pcap quarantine (#9) and the must-deliver data seam (#13) are both preserved by
  construction, not by review vigilance. Added in this ADR's commit (the same-commit rule).
- Invariants #9 and #13 are **reinforced, not superseded**: this ADR adds a fact that respects both,
  so no existing ADR changes status.
- The screen renderer's device implementation must draw the CAPTURED label; that is a live-doc render
  change shipped in the same slice.
