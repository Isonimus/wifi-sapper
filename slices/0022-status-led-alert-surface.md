---
id: '0022'
title: "Status-LED alert surface over a new surface event bus"
type: slice
status: accepted
date: 2026-09-19
supersedes: []
superseded_by: []
---

## Goal

Give the headless appliance its first real operator-facing surface — a status LED — and build the shared
mechanism every later surface will use to get there: the surface **event bus** (ADR-0021, enacting
ADR-0001's observers-never-owners boundary). The engine already produces the facts (drain outcomes, sync
outcomes, recovered passwords); until now they reached only a diagnostic serial log and a pull-snapshot
getter. This slice publishes them onto a synchronous, heap-free multicast bus, and adds the first
subscriber: a clock-driven `LedStatusSurface` that maps those facts to a semantic `LedStatus` and drives
the board's WS2812 through an abstract `LedDriver` seam.

The point of a status LED on a device with no screen, keyboard, or watcher is a glance-able answer to "is
it alive, and did it do anything?" So the base states **blink** — a heartbeat that proves the firmware is
still looping, where a solid colour could be a hung board — and a freshly recovered password latches a
solid **flash** that cannot be missed. This is slice-7's first sub-slice; the web dashboard, screen toast,
and push webhook are later sub-slices, each an additional bus subscriber that touches no engine code.

## Definition of Done

**Scenario A — the bus delivers every event to every subscriber, in subscription order (host).**
- **Given** an `EventBus` with three subscribed sinks
- **When** an event is published
- **Then** all three sinks receive it, in the order they subscribed — a surface can rely on seeing every
  fact (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_event_bus`.

**Scenario B — a full or duplicate subscription fails loud, never silently drops or doubles a surface (host).**
- **Given** an `EventBus` filled to capacity, and separately a sink already subscribed
- **When** one more sink subscribes, and the already-subscribed sink subscribes again
- **Then** both `subscribe()` calls return `false` (the boot wiring treats that as fatal), the
  subscriber count is unchanged, and a re-subscribe does not double-deliver — a missing or doubled surface
  is a caught wiring bug, not a silent one (ADR-0021, §3).
- **Proof:** `npm run test:native` → `test/test_event_bus`.

**Scenario C — typed payloads route to the right field and are copied by the receiver (host).**
- **Given** a recording sink on a bus
- **When** each event kind is published (drain started/completed, sync completed, new password, first-sync
  summary) carrying its payload
- **Then** the sink reads each payload from its correct field and copies it out — proving the
  borrowed-pointer contract (payloads valid only during dispatch) and that a surface copies what it keeps
  (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_event_bus`.

**Scenario D — at boot the LED lights a hunting heartbeat that blinks (host).**
- **Given** a fresh `LedStatusSurface` over a fake driver
- **When** `begin()` runs and the clock advances across heartbeat half-periods
- **Then** the driver is shown `Hunting`, then `Off`, then `Hunting` — the base status blinks as proof the
  loop is running, not a solid colour that could be a hang (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario E — drain facts map to working / hunting, and offline maps to degraded (host).**
- **Given** a running surface
- **When** a `DrainStarted` fact arrives, then an associated `DrainCompleted`, then one that could not
  associate
- **Then** the LED shows `Working` for the open window, returns to `Hunting` once it associated, and shows
  `Degraded` only when it could not associate — `Degraded` is the connectivity signal ("it can't reach the
  network"), the one thing an operator must act on (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario F — a healthy sync-only window, or an online cycle with data-level problems, stays hunting (host).**
- **Given** a running surface
- **When** a `DrainCompleted` arrives from a sync-only window (associated, every count zero), and separately
  from an associated cycle that had a rejected upload, a purged corrupt capture, and a store error
- **Then** both show `Hunting`, not `Degraded` — an hourly sync with no uploads is normal, and per-capture
  data outcomes are routine/self-healing and do not mean the appliance is offline; painting the coarse LED
  amber on a routine rejection would make a healthy board read offline almost always (ADR-0021; slice-0022
  Scenario J on-air finding). Those details are surfaced in the serial log and the later web dashboard.
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario G — a hard resume fault sits solid, not blinking (host).**
- **Given** a running surface
- **When** a `DrainCompleted` reports `resumeFailed`
- **Then** the LED shows `Fault` and holds it solid across a heartbeat dark-half (no blink to `Off`) — an
  alarm reads differently from a heartbeat (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario H — a new password latches a solid flash, overriding the heartbeat, then releases (host).**
- **Given** a running surface in its hunting heartbeat
- **When** a `NewPassword` fact arrives and the clock advances through and past the hold
- **Then** the LED shows solid `Recovered` (overriding the blink — no `Off` during the hold), and after the
  hold elapses returns to the hunting heartbeat — a recovery you cannot miss, that does not stick forever
  (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_led_status_surface`.

**Scenario I — the producers actually publish their facts on the bus (host).**
- **Given** the `UploadSupervisor` and `CrackedSync` wired to a bus with a recording sink
- **When** a drain cycle runs and a sync completes
- **Then** the supervisor publishes exactly one `DrainStarted` as it opens the window and one
  `DrainCompleted` carrying the same outcome its `lastDrain()` snapshot exposes, and the sync publishes its
  `SyncCompleted` / `NewPassword` / `FirstSyncSummary` facts in the order ADR-0019 defined — the migration
  from the old observer/getter seams onto the bus preserves behaviour (ADR-0021).
- **Proof:** `npm run test:native` → `test/test_upload_supervisor`, `test/test_cracked_sync`.

**Scenario J — on real hardware the LED is driven through its states over the live spine (device).**
- **Given** a `cardputer_testhooks` build flashed with real credentials and `SAPPER_TEST_LED=1`
- **When** the LED probe runs the shipped loop, forces STA windows, and injects a synthetic new-password
  fact through the bus
- **Then** the Esp32 driver logs `[LED] status=working`, `=hunting`, and `=recovered`, no `[FATAL]`/`[ERROR]`
  appears, and the physical WS2812 shows blue, a green heartbeat, and a white flash — the bus is really
  wired into the shipped firmware and the LED really lights (ADR-0004 lane 3; §4 invariant #5).
- **Proof:** `npm run verify:led` (artifact `artifacts/0022-led.txt`).

## Design

**The bus (ADR-0021).** `core/event_bus.h` defines a tagged `AppEvent`, an `EventSink` with one
`onAppEvent`, and a fixed-capacity `EventBus` whose `publish()` fans out synchronously in subscription
order. Payloads are borrowed const pointers valid only for the dispatch call. The bus carries
surface-facing *facts* — `DrainStarted`, `DrainCompleted`, `SyncCompleted`, `NewPassword`,
`FirstSyncSummary`; the capture→queue data path stays a direct `CaptureReadyObserver` seam and is never on
the bus (a dropped fact is cosmetic; a dropped capture is a lost handshake).

**Producer migration.** `CrackedSync` publishes its three facts to an injected `EventBus&` (the old
`SyncEventObserver` interface is removed); `UploadSupervisor` gains an optional `EventBus*` and publishes
`DrainStarted`/`DrainCompleted`, while keeping `lastDrain()`/`lastSync()` as the pull-snapshot the on-air
verifies poll. The serial diagnostic logger becomes a `SerialEventLogger` EventSink emitting the same
`[SYNC]`/`[CRACK]` lines, now bus-sourced.

**The LED.** `surface/led_driver.h` is the HAL seam: a semantic `LedStatus` (Off/Hunting/Working/Degraded/
Fault/Recovered) and an abstract `LedDriver::show()`. `surface/led_status_surface.h` is the pure,
clock-driven policy — an `EventSink` that keeps a small state machine and, on `tick(now)`, renders the
current status (base heartbeat blink, solid Fault, latched Recovered flash), calling the driver only on a
change. `surface/led_driver_esp32.*` renders `LedStatus` onto the active board's LED (WS2812 via the
Arduino-ESP32 `rgbLedWrite`, from the Board Profile's `LedKind`/`ledPin`), and logs each status for the
verify. All policy is host-tested against a fake driver and a fake clock; only the physical colour is
device-verified.

## Verification

- **Scenarios A–C** (bus): `npm run test:native` → `test/test_event_bus`.
- **Scenarios D–H** (LED policy): `npm run test:native` → `test/test_led_status_surface`.
- **Scenario I** (producer publishing): `npm run test:native` → `test/test_upload_supervisor`,
  `test/test_cracked_sync` (and `test/test_sync_session` for the cadence over the migrated seam).
- **Scenario J** (on-air): `npm run verify:led` on an attached Cardputer ADV, artifact
  `artifacts/0022-led.txt`.
- **Board compile** (lane 2): `npm run build:boards`.

## As built

- **Wrap-safe deadline helper extracted at the rule of three.** The LED heartbeat/latch is the third
  caller of the `reached(now, deadline)` wrap-safe test (after HuntEngine and UploadSupervisor, whose own
  comments named the third caller as the extraction point). It now lives once in `core/deadline.h`, and
  both prior copies were refactored to include it — a DRY cleanup this slice triggered rather than a fourth
  copy.
- **Driver log deduped to semantic changes.** The heartbeat toggles the display Off/on every second; the
  Esp32 driver renders every call to the LED but logs `[LED] status=` only on a genuine semantic change, so
  neither the verify nor the shipped serial is buried under a line per second.
- **`SyncEventObserver` removed, not kept alongside the bus.** The user chose the unified bus over deferring
  it, so the bespoke observer interface was deleted and `SerialSyncObserver` became a `SerialEventLogger`
  EventSink — one mechanism, as ADR-0001 intended, not two.
- **Getters retained as the harness pull-snapshot.** `lastDrain()`/`lastSync()`/the count getters stay for
  the on-air verifies (push for surfaces, pull for tests); the DrainOutcome is computed once and both
  published and retained, so there is no second source to drift.
- **Test-hooks stimulus `huntLoopInjectCrackedAlert()`.** A real steady-state crack cannot be forced on
  demand, so the LED verify publishes a synthetic new-password fact through the *same bus* a real crack uses
  (§4 invariant #2), gated behind `SAPPER_TEST_HOOKS` (§4 invariant #4).
- **LED hardware from the Board Profile.** The Cardputer ADV's WS2812 on GPIO 21 was already documented in
  `config/boards/cardputer.h` (`LedKind::Rgb`, `ledPin = 21`) — the driver reads it there and hardcodes no
  pin, so a new board is a profile change.
- **Adds §4 standing invariant #13** (surfaces subscribe to the bus; the engine never calls a surface; the
  capture path stays off the bus), in the same commit as ADR-0021.
- **Host lanes measured:** `test_event_bus` 5, `test_led_status_surface` 9, `test_upload_supervisor` 13
  (added the drain-publishes-facts test), `test_cracked_sync` 7 and `test_sync_session` 2 (migrated to the
  bus), all green; board compile green.
- **Adversarial pass (ADR-0017) findings:** the `[env:native]` `build_src_filter` was missing
  `+<surface/led_status_surface.cpp>`, so the LED policy suite could not link on the native lane (CRITICAL,
  fixed — the manual g++ runs masked it; `pio test` is the real gate); a stale `cracked_sync.h` file comment
  still named the removed `SyncEventObserver` (LOW, fixed). The MEDIUM (`statusFromDrain` painting a
  purged/rejected cycle non-green) was first applied, then **superseded by the on-air Scenario J finding
  below** — the review was right that green-on-data-loss is a gap, but wrong about the fix.
- **On-air Scenario J redesigned the `Degraded` meaning (connectivity, not data outcomes).** The first
  Scenario J run showed every drain completing `degraded` while `[SYNC] download ok` proved the board was
  associating fine: the amber came from routine `rejected` uploads (partial captures, wpa-sec dedup) on a
  live-hunting board, so the LED never returned to `hunting`. Painting the coarse status amber on every
  rejection makes a healthy, online appliance read offline almost always. `statusFromDrain` was rescoped to
  `Degraded = could not associate`; `rejected`/`purged`/`storeErrors` no longer touch the LED (they are
  routine/rare, self-healing, and surfaced in the serial log and the later web dashboard). This is the
  actionable one-colour signal an operator needs — "can it reach the network?" — and it is why the on-air
  lane exists: a unit test could not have caught that the mapping was wrong for real traffic.
