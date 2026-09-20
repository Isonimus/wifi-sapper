# WiFi Sapper — Project Conventions

The WiFi Sapper is an autonomous, portable ESP-32 firmware appliance spun off from
The Adversary (`../adversary`). It runs WPA/WPA2 handshake auto-hunt **endlessly**; on each
new capture it auto-uploads the handshake to `wpa-sec.stanev.org`, and once an hour it
fetches cracked results back and announces any newly recovered passwords. Unlike its
interactive parent, the Sapper is **headless-first**: the capture→upload→sync engine has no
UI dependency, and every surface (optional display, LED, web dashboard, push webhook) is an
event-bus subscriber — so the same firmware runs on hardware with or without a screen,
keyboard, or SD card.

Stack: C++17 on the Arduino framework over ESP-IDF (pioarduino platform + Bruce's patched
esp32-arduino libs, required for raw deauth TX), built with PlatformIO. LovyanGFX for display
(auto-detect across M5Stack, CYD, Lilygo, and generic panels). ArduinoJson; Unity for native
unit tests. First reference target: **M5Stack Cardputer ADV** (ESP32-S3, no PSRAM — the
Adversary repo labels this same hardware plainly as "Cardputer").

General working practices — quality bar, testing standard, commit hygiene, delegation,
correction, language — live in [`docs/quality-bar.md`](docs/quality-bar.md) and apply here
without being restated. It ships with the method and is vendored into this repo, so it is
yours to adapt; an update keeps what you changed (stele:ADR-0024).

This file carries only what is specific to **this** repo. Restating a rule from the bar here
would create a second copy with no sync path, which is the failure stele:ADR-0005 exists to
prevent.

## 1. Document taxonomy — four kinds

Every document is exactly one of four things.

| Kind | Files | Rule |
|---|---|---|
| **Immutable** | `adr/*.md`, `slices/*.md` | Written once. Body prose is never edited. Only status/supersession fields may change. |
| **Generated** | `adr/INDEX.md` | Built by script from frontmatter. Never hand-edited. |
| **Ledger** | `LEDGER.md` | Exactly one per repo. The only hand-maintained tracker. |
| **Live doc** | `README.md`, `docs/*.md` | Describes how something behaves *now*. Updated **in the same change** as the thing it describes, and cited from this file so it is never orphaned (stele:ADR-0010). |

- **ADR** — a decision later work must obey (a mechanism, data format, or boundary).
  Asserts *"on date X we chose Y because Z"*: a historical claim, true forever.
- **Slice** — one feature work-unit. Written before implementation, **frozen at merge**
  and rewritten to past tense: *"this is what shipped."* Freezing converts it from a
  current-state claim (always going stale) into a historical one (never stale).
- **README.md** — live document; update it in the same change that alters any
  user/dev-facing feature or API. It is the repo's operator/dev entry point — the build and
  verify commands and the first-boot provisioning guide (ADR-0006) — cited here so it is not
  orphaned (stele:ADR-0010).

**Single writer, one direction.** An ADR records a deferral *once*, as a fact about that
decision. `LEDGER.md` cites the ADR. **Never reach back into an ADR to close a ledger
item.** Closing an item means deleting its line from the ledger.

Changing our minds means writing a **new** ADR that supersedes the old one, never editing
it. The superseding note must say *why the old reasoning was wrong* — that record is the
most valuable thing this workflow produces, and an in-place edit destroys it.

A committed document's body may **gain** lines — an appended `## Amendment — <date>: …`, or a
correction marker placed at the claim it corrects — and may never lose or rewrite one. The
hook enforces this (stele:ADR-0019); frontmatter is exempt, because status and supersession
fields are how a record announces it was superseded.

This is also how a justified rule-violation gets recorded. `~/.claude/CLAUDE.md` §2 says a
justified violation is written down as a decision rather than taken as a silent exception;
in this repo, that decision is a new or superseding ADR.

## 2. Enforcement — invariants are executable

`node scripts/lint-docs.mjs` runs from a pre-commit hook and in CI. The hook checks the
**commit**, not the working tree, so a fix you forgot to stage cannot green a red commit
(stele:ADR-0018); alongside the linter it verifies that `adr/INDEX.md` matches the corpus and
that immutable bodies have only gained lines (stele:ADR-0019).

A citation is bare (`ADR-NNNN`) only when it means *this* repo, and qualified
(`<repo>:ADR-NNNN`) otherwise (stele:ADR-0009). The linter resolves citations in `LEDGER.md`,
in the corpus, and in the prose that is read as instruction — this file, `README.md`,
`docs/`, `.claude/commands/` — so a citation that rots there fails the build rather than
quietly misrouting the next session (stele:ADR-0020).

A rule enforced by memory is a rule that holds until the first busy afternoon. If a
convention matters, it gets a rule; if it genuinely can't be checked, say so out loud
rather than writing it down and trusting it.

Run `/wrap-up` before finishing a task.

## 3. Verification harness — measure, don't assume

Some changes cannot be asserted in a unit test: rendering, world generation, physics,
timing, anything whose correctness is "does it look and behave right at runtime". The
answer is **not** to skip verification and eyeball it once by hand.

**Any slice whose behaviour a unit test cannot assert ships a verification script.**

`scripts/<slice>-verify.mjs` — drives the real system headlessly, exercises the specific
behaviour the slice claims, and:

- **fails on any console error or page error** — this half is pass/fail and machine-checkable;
- **writes artifacts** (screenshots, dumps, measured numbers) for human review — this half
  needs eyes, and that is fine, as long as the first half still runs unattended;
- **is named in the slice's `## Verification` section**, so the claim and its evidence are
  linked;
- **is wired into `package.json`**, and its error-check half runs in CI.

That last point is the one that gets skipped, so it is **rule-checked** (R11): the linter
fails if any `scripts/*-verify.mjs` is absent from `package.json`. A verify script that is
not wired runs exactly once, on the day it was written, and is dead thereafter — it
documents that the slice worked once, which is not what a regression check is for. Probes
are exempt: a probe answers its question once and the number lands in an ADR.

Every slice carries two required sections, both rule-checked: `## Verification` names the
proof (R12), and `## Definition of Done` states the acceptance criteria as Given/When/Then
scenarios written before the code (R13, stele:ADR-0011). Each scenario names its proof in
`## Verification`; the linter checks the sections exist and that the Definition of Done
holds a full triad — it cannot check that a scenario is *right*, which is what `/wrap-up`
is for.

Probes are the same tool used before the fact: when a design question has a measurable
answer (how many caves per chunk, what the frame cost is), write `scripts/<topic>-probe.mjs`
and put the **measured numbers** in the ADR. Design decisions cite data, not estimates.

## 4. Standing invariants

Repo-specific definition-of-done rules. A slice is not complete until it satisfies every
one that applies. Each cites the ADR that created it; exceptions are listed with the
reason, so nobody "fixes" a deliberate choice.

These live **here, in the repo** — not in assistant memory. A rule that governs the
codebase must be greppable, diffable, reviewable, and survive a change of machine
(stele:ADR-0005).

Each row declares how it is **enforced**, so an aspiration is never mistaken for a
guarantee:

- `verified_by: <script>` — a wired verify script or lint rule checks it;
- `pending (LEDGER)` — enforceable but not yet enforced; a ledger item carries the debt;
- `review-only` — enforceable only by human judgement, so `/wrap-up` is the enforcement.

The declaration is a convention checked at review, not by the linter — the linter does not
read this file (stele:ADR-0004). What it *does* enforce is that every `verified_by` script is
actually wired (R11), so a row cannot claim a check that runs nowhere.

| # | Invariant | Source | Enforced by |
|---|---|---|---|
| 1 | Serial observation commands (`ping`, `state`, event tap) mutate no engine state — they expose only what the device already publishes or logs. | ADR-0003 | review-only |
| 2 | Injected stimulus enters the engine only through the same seams a real event uses — never a parallel test-only path. | ADR-0003 | review-only |
| 3 | Every state that blocks before the engine loop runs (e.g. first-boot captive portal) pumps the serial channel. | ADR-0003 | verified_by: scripts/0007-provisioning-verify.mjs |
| 4 | The actuation/stimulus vocabulary is absent from any build without `SAPPER_TEST_HOOKS` — no shipped binary can be commanded to deauth or upload over serial. | ADR-0003 | pending (LEDGER) |
| 5 | No cloud-CI lane depends on attached hardware — device-in-the-loop verification runs on a hardware lane and its artifact is committed for review. | ADR-0004 | pending (LEDGER) |
| 6 | Provisioned secrets (WiFi credentials, wpa-sec key) are read and written only through the `persistProvisioning()` NVS seam — never scattered across call sites. | ADR-0006 | review-only |
| 7 | No build's serial vocabulary can write or clear provisioning credentials — provisioning is never a serial actuation path. | ADR-0006 | review-only |
| 8 | All 802.11/EAPOL frame interpretation lives in the pure capture core, compiled and unit-tested on the native lane; the RF sniffer/TX seams forward raw bytes only and carry no protocol logic. | ADR-0009 | review-only |
| 9 | Capture artifacts (pcap bytes) are emitted only through the `CaptureSink` seam — no module opens or writes a capture file directly. | ADR-0009 | review-only |
| 10 | The promiscuous RX callback forwards raw frame bytes to the pure core and does nothing else — no allocation, blocking, or protocol interpretation in the Wi-Fi-driver callback context. | ADR-0011 | review-only |
| 11 | A consumer is switched under a live `RadioSniffer` only via the atomic router the engine holds as the sniffer's single consumer for its started lifetime; a sink is reset or destroyed only after a quiesce settle with the router aimed away from it — never by a `stop()`/`begin()` re-attach, which cannot hard-join an in-flight callback. | ADR-0015 | review-only |
| 12 | Cracked passwords are read and written only through the `CrackedManifest`/`CrackedStore` seam — no surface parses a results file or the download body directly. Mirrors #9 (captures via `CaptureSink`) and #6 (secrets via the provisioning seam), so slice-7's several surfaces cannot each grow their own store parse. | ADR-0019 | review-only |
| 13 | Surfaces receive engine facts only by subscribing to the `EventBus` (one `EventSink::onAppEvent`); the engine never calls a surface directly, and no surface reaches into engine state except the pull snapshot the on-air verifies use. The capture→queue data path stays a direct `CaptureReadyObserver` seam and is never on the bus (a dropped fact is cosmetic; a dropped capture is a lost handshake). Enacts ADR-0001's observers-never-owners boundary. | ADR-0021 | review-only |
| 14 | A *transmitting* surface (one whose reaction needs the network) observes facts on the `EventBus` like any surface but transmits only inside a supervisor-owned STA window, via the `WindowNotifier` seam the supervisor flushes (`setNotifier`) — never blocking IO inside `onAppEvent` (which would stall the synchronous bus dispatch). It reaches connectivity only through that seam, not by opening its own radio path. The push webhook is the first; a payload it sends off-device carries no plaintext PSK (the notification type has no password field). | ADR-0023 | review-only |
| 15 | Deauth/disassoc frame *construction* lives in the pure `deauth` core, compiled and unit-tested on the native lane; the `RawTransmitter` seam forwards fully-formed frame bytes only and carries no frame-building logic (the TX mirror of #8's read-side purity and #9's raw-bytes RX seam). *(This row's former `SAPPER_TEST_HOOKS` compile-gate clause was superseded by ADR-0029; the gating is now #16.)* | ADR-0027 | review-only |
| 16 | The raw-TX *capability* (the device `esp_wifi_80211_tx` path and the `ieee80211_raw_frame_sanity_check` SDK bypass) ships in every device binary, but a deauth frame is transmitted only when the operator has explicitly *armed* deauth through the provisioning toggle (`ProvisioningRecord.deauthEnabled`, default **off**): `hunt_loop` injects the `RawTransmitter` into the `HuntEngine` only when armed, and the engine transmits (broadcast deauth/disassoc at `capturingBssid()` during Capturing) only when it holds a transmitter — a disarmed or unprovisioned device transmits nothing. Supersedes #15's former `SAPPER_TEST_HOOKS` compile-gate (ADR-0027 decision 3): the gate moved from compile-time absence to a runtime, operator-armed default-off, because the appliance's authorized-industrial purpose needs deauth in the shipped loop while a lost/misused unit must not jam by default. | ADR-0029 | verified_by: scripts/0030-deauth-loop-verify.mjs (shipped-armed TX) + test/test_hunt_engine (disarmed = no TX); review-only (portal/hunt_loop wiring) |

When an ADR's consequences create a rule that all *future* work must follow, add the row
in the same commit as the ADR.
