---
id: '0072'
title: "Bounded re-verification cadence for rotated PSKs"
type: architecture
status: accepted
date: 2026-10-02
supersedes: []
superseded_by: []
---

## Context

Four gates skip a network the appliance already knows, so the endless hunt does not spend airtime
or an upload on a handshake that adds nothing:

- **Gate A** (ADR-0059, §4 #27) — the `UploadSupervisor` drain drops a queued capture whose BSSID is
  already **cracked** (`CrackedLookup::isCracked`).
- **Gate B** (ADR-0061, §4 #28) — `HuntEngine` target selection skips an already-**cracked** BSSID.
- **Gate C** (ADR-0065, §4 #30) — both the drain and target selection skip an already-**submitted**
  BSSID (`SubmittedLookup::isSubmitted`), the general form of A/B (cracked ⊆ submitted): a network is
  skipped from the moment a valid handshake lands at wpa-sec, cracked yet or not.

Each gate keys off **membership**, which is permanent until LRU eviction at the manifest bound. That is
correct for a static network, but it has a hole every one of those three ADRs recorded as a deferred
limitation (LEDGER): **a rotated PSK is never re-recovered.** Once a BSSID is in the cracked or submitted
set, it is never re-captured, so a fresh handshake for its *new* PSK never reaches wpa-sec, so wpa-sec
never re-cracks it, so the `CrackedManifest::apply` `Changed` path (the rotated-PSK alert, ADR-0019
decision #5) never fires. The appliance silently goes stale against the exact event — a key rotation —
that an authorized-pentest appliance most wants to catch.

The fix the three ADRs deferred: **re-verify a known network at most once per long interval.** Let a
stale entry eventually become targetable and uploadable again so a fresh handshake flows to wpa-sec,
then re-suppress it on the fresh submission — keeping the constant re-work the gates exist to prevent,
while letting a rotation surface within a bounded delay.

The deferral was blocked on a clock. The two timestamps the manifests already hold —
`CrackedEntry.firstSeenCrackedMs` and `SubmittedEntry.uploadedAt` — are `millis()` values, eviction
keys only, reset at every reboot, and explicitly documented as **not** re-verification timers
(submitted_store.h). A measurement of the firmware's clocks (the slice-0073 time survey) confirmed:

- The **only** real wall-clock is NTP epoch, set by `syncClock()` in wifi_station.cpp, available in RAM
  after the first STA window and used today solely as a TLS precondition (`kMinValidEpoch = 1577836800`,
  2020-01-01, is its "is this a real synced time, not the 1970 power-on default" threshold). It is never
  persisted and never threaded into the pure logic.
- **`bringUpStation()` guarantees that epoch is valid inside every drain/sync window** — it associates,
  calls `syncClock()`, and refuses to proceed if the clock is not real (station_control_esp32.cpp). So
  the one context that does network work is the one context where real time is guaranteed.
- In the engine's target-selection path (un-associated, promiscuous) epoch is only *best-effort*: 1970
  on a fresh boot, valid only after the first STA window.
- The `millis()`-based hourly sync cadence and every manifest timestamp reset at boot — nothing about
  the cadence survives a power cycle.

So the missing piece is a **persisted wall-clock timestamp** per entry, keyed off real epoch, read in a
context that actually has valid epoch.

## Decision

Add a bounded re-verification cadence keyed on a new persisted per-entry wall-clock timestamp. The
cadence re-enables *all four* gates for one entry once it is stale, then re-suppresses it on the fresh
re-submission.

1. **A new persisted field `lastVerifiedEpoch` (uint32_t, epoch seconds) on both `CrackedEntry` and
   `SubmittedEntry`,** distinct from the existing `millis()` eviction keys (which keep their role). It
   records the real time the appliance last *verified* the entry — last recorded it submitted
   (submitted set) or last cracked/re-cracked it (cracked manifest). `0` means "never verified in
   wall-clock terms" (a first-boot/first-window entry, or a record migrated from the pre-cadence format).

2. **An entry is skipped only while it is a member AND fresh.** `isCracked`/`isSubmitted` keep their
   exact signatures — membership stays the *question* the gates ask — but now answer `false` for a
   **stale** member (`now − lastVerifiedEpoch ≥ kReverifyIntervalSec`). A stale member is therefore
   targetable (gate B / gate C) and uploadable (gate A / gate C drain) again, exactly as if it were
   unknown, for one re-verification pass.

3. **The clock is cached in the manifest, fed from the device, read only where epoch is guaranteed valid
   — Fork D (gates and engine untouched).** Each manifest gains `setVerificationClock(uint32_t
   nowEpoch)` caching the current epoch; the freshness test reads that cache. `hunt_loop` (device code)
   feeds it `std::time(nullptr)` each loop. The freshness test is **guarded by `kMinRealEpoch`**: while
   the cached epoch is below the real-time threshold (before the first NTP sync), no entry is ever stale,
   so the gates behave exactly as before the cadence existed. This confines the new wall-clock dependency
   to a cache the device fills, keeps `CrackedLookup`/`SubmittedLookup` and `HuntEngine` free of any time
   parameter or second clock domain (the engine knows only `millis()`), and so preserves §4 #8/#10 by
   construction: the manifests were already app-task-only and never on the `onFrame` path, and nothing
   about that changes.

   The considered alternative — **Fork A, time-aware gate signatures** (`isCracked(bssid, nowEpoch)`,
   threading epoch into the engine's target selection and the drain) — was rejected: it spreads a second
   clock domain into the engine's best-effort, un-associated path and widens the lookup seams, buying
   continuous freshness re-evaluation that a days-long interval has no use for.

4. **The cadence bumps `lastVerifiedEpoch` on a real verification event, and only then:**
   - **Submitted set** — `markSubmitted` stamps the entry on *every* terminal Accepted/Duplicate upload,
     including a re-mark of a held BSSID. This is the reset that re-suppresses a just-re-verified
     network. (`uploadedAt` still keeps its original value — the two fields now have distinct roles:
     eviction ordering vs re-verification.)
   - **Cracked manifest** — `apply` stamps the entry on `New` and `Changed` (a real crack or re-crack),
     and **must not stamp on `Unchanged`.** `Unchanged` is the hourly re-download of a known crack; if it
     re-stamped, the cracked timer would reset every hour and nothing would ever go stale. This is the
     subtle correctness point of the whole mechanism.

5. **Why both manifests carry the timer, not the submitted set alone.** After a re-verification the
   freshly-stamped *submitted* entry re-suppresses the network even while the cracked entry's timer stays
   stale (an `Unchanged` re-crack never bumps it) — target selection skips on `cracked OR submitted`, so
   the submitted side carries the suppression and the pair converges to **exactly one re-verification per
   interval** driven by the submitted timer. But the submitted set is **not durable** (it is local-only
   and lost to a flash wipe; the cracked set is re-seeded from wpa-sec on every sync and survives — §4
   #30). Were only the submitted set timed, a post-wipe re-seeded crack would have no timer and gate B
   would suppress it forever again. So the cracked entry carries its own timer, stamped fresh by
   `apply(New)` on re-seed (no post-wipe re-verification storm), and stale an interval later.

6. **Interval: `kReverifyIntervalSec = 604800` (7 days), an architecture-derived default.** Short enough
   to catch a rotation within a week, long enough that the re-work is one extra capture+upload per known
   network per week (negligible against the endless hunt). It is one constant for both sets (KISS), a
   constexpr beside the cadence policy, not a `HuntConfig`/provisioning field — a measured tuning pass
   would supersede the value, not make it runtime-configurable (as with `kRssiFloor`, dwell, settle, the
   deauth cadence, and the backoff constants). Tuning against observed real-world rotation periods is
   deferred (LEDGER), like those others.

7. **On-disk migration: the LittleFS record grows by four bytes, so the store magic bumps (`CRK1→CRK2`,
   `SUB1→SUB2`) and the loader still accepts the old magic,** reading a v1 record at its old length and
   defaulting `lastVerifiedEpoch = 0`. A v1 file is thus read without loss and rewritten in the v2 format
   on the first flush. A `0`-stamped (migrated, or first-window) entry becomes stale as soon as a valid
   epoch exists, so the **first run after a firmware upgrade re-verifies the existing known set once** —
   a deliberate, bounded, self-correcting burst that also establishes the first real wall-clock baseline.
   An unknown magic is still a loud load failure (quality bar §3), never a silent reinterpret.

## Consequences

- A rotated PSK is now re-recovered within `kReverifyIntervalSec + O(one hunt cycle + one sync)`: the
  entry goes stale, the hunt re-captures it, the drain re-uploads it, wpa-sec re-cracks the new key, and
  the next sync's `apply` returns `Changed` and fires the rotated-PSK alert (ADR-0019 decision #5) that
  was previously unreachable. The three deferred limitations (ADR-0059/-0061/-0065) are closed.
- The re-work is bounded and self-limiting: each known network costs one extra capture+upload per
  interval, then is suppressed again. All-stale never spins (the gates still fall through to a fresh
  discovery sweep, as for all-cracked/all-submitted).
- A known network whose PSK never rotates is re-verified once per interval forever (the submitted timer
  drives it; the cracked timer stays stale after the first interval because `apply(Unchanged)` never
  bumps it, which is harmless — the fresh submitted entry carries the suppression). This is the intended
  "re-verify at most once per long interval," not a defect.
- Wall-clock enters the pure layer for the first time, but only as a device-fed cache with a
  `kMinRealEpoch` guard: before the first NTP sync, and on a device that never associates, the cadence is
  inert and the pre-cadence gate behaviour holds exactly. An appliance that never reaches wpa-sec never
  re-verifies — moot, since it also never re-uploads or re-syncs.
- A capture first submitted (or first cracked) in the *very first* post-boot STA window stamps a stale
  epoch, because NTP goes valid mid-drain while `hunt_loop` fed the cache 1970 at the top of that
  iteration; such an entry re-verifies once, immediately, then settles with a valid stamp. Bounded to
  first-window entries, self-correcting, and irrelevant against a 7-day interval.
- The exact interval is one field run away from measured; the LEDGER tuning item carries that debt, as
  for the other architecture-derived cadence defaults.
- `kMinRealEpoch` (the pure valid-epoch threshold) duplicates the value of wifi_station.cpp's device-only
  `kMinValidEpoch` TLS threshold. Left as two instances deliberately (rule of three — this is the
  second), matching the repo's other same-on-purpose duplications; hoist to one home only if a third
  user appears.
