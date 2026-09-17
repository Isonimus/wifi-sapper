---
id: '0003'
title: "A serial control channel for the verify harness, split by transmit risk"
type: architecture
status: accepted
date: 2026-09-17
supersedes: []
superseded_by: []
---

## Context

`docs/quality-bar.md` §3 requires that any behaviour a unit test cannot assert ships a
verification script driving the *real* system headlessly. The Sapper is almost entirely
such behaviour: an autonomous engine hunting, capturing, uploading, and syncing against
hardware and a remote service. A firmware that only *writes* to the serial port can be
asserted on what it volunteers and nothing more — the limit `dupin` measured in its
`dupin:ADR-0009` before adding a reader in `dupin:ADR-0014`. We adopt that reader here, but
two facts about the Sapper change its shape:

- **There is almost no keyboard control flow to shim.** ADR-0001 made the Sapper
  headless-first with a *read-only* scanner view. `dupin`'s channel injects keypresses
  through the UI dispatch because `dupin` is a menu-driven tool; the Sapper has no such
  dispatch to drive. What must be observed and driven here is the **HuntEngine and the
  network flows**, not keystrokes.

- **`dupin`'s safety argument does not transfer.** `dupin:ADR-0014` decision #2 rests
  explicitly on *"Dupin has no transmit path, so the channel cannot be turned into one."*
  The Sapper's entire purpose is an autonomous **transmit** path (deauth) plus **upload**
  (handshake exfiltration to wpa-sec). An unflagged actuation channel would therefore ship a
  remotely-commandable deauth/upload capability in the production binary.

The authorisation half of `dupin`'s argument *does* transfer: physical USB already permits
reflashing, so a port-only channel grants an attacker with the port no capability they lack.
But that licenses only **observation**. It does not license shipping **actuation**, and the
reason we need actuation at all is narrow and technical: an autonomous device cannot be
verified headlessly without injected stimulus — there is no real access point in CI to emit a
handshake, and no wpa-sec result to fetch. Stimulus that drives the real deauth/upload path
must exist for the harness, and must **not** exist in the shipped binary.

## Decision

**1. The firmware reads the port in every build, for the observation half.** Not behind a
flag — a flag would mean the observation gate proved a binary nobody ships, the failure
ADR-0002's toolchain pinning and `docs/quality-bar.md` §3 both exist to prevent. (Carried
from `dupin:ADR-0014` #1.)

**2. The vocabulary is split by risk.**

- **Observation — unflagged, in every build.** `ping` (liveness); `state` (engine phase,
  current target BSSID/SSID/channel, free heap and largest contiguous block, WiFi state,
  upload-queue depth, wpa-sec status counts); and an opt-in event tap mirroring the
  `core/event_bus` lines. All read-only: they expose only what the device already publishes
  or logs, and mutate no engine state.

- **Stimulus / fault injection — behind `SAPPER_TEST_HOOKS`, compiled out of release.**
  Inject a synthetic handshake through the real capture seam; stub the next wpa-sec upload or
  download response; force `time-not-synced` or `heap-too-low`; advance the hourly-sync
  clock. These drive the real engine deterministically for the verify harness and are
  **absent from the shipped binary** — no production Sapper can be told over serial to deauth
  or upload against a chosen target.

**3. One path per behaviour, reached through a seam — never a parallel copy.** Observation
reads the same state the surfaces read. Stimulus enters the engine at the *same seams a real
event uses* — a synthetic handshake goes through the same capture-complete path a sniffed one
does — so the harness drives the code that ships, not a divergent test-only path. (Carried
from `dupin:ADR-0014` #3.)

**4. Parsing is pure and host-tested; dispatch is device behaviour and is gated.** The line
grammar and accumulator are pure functions with host unit tests; the dispatch that touches
the engine, display, or network is covered by a wired `scripts/*-verify.mjs`. This is the
§3 split. (Carried from `dupin:ADR-0014` #5.)

**5. Bounded both directions; refuse, do not truncate.** A line past the max length is
refused, not shortened (truncating an actuation command into a shorter valid one would run
something unasked). At most a bounded number of bytes drain per loop pass, so a flood cannot
starve the capture path. (Carried from `dupin:ADR-0014` #6.)

**6. Replies are `[CMD]` and `[STATE]`, never `[ERROR]`/`[FATAL]`.** Those two are the
patterns the harness fails on; a refused command is not a firmware fault and says so rather
than being dropped silently. (Carried from `dupin:ADR-0014` #7.)

**7. Every blocking pre-engine state pumps the channel.** `dupin` found that a modal menu in
`setup()` left `loop()` — and therefore any channel served from it — unreachable until a
human pressed a key. The Sapper is headless-first so its engine loop runs from boot, but the
first-boot captive-portal provisioning state (its own forthcoming ADR) is a pre-engine mode a
freshly-flashed device sits in, and the harness must be able to drive a just-provisioned
device. So: any state that blocks before the engine loop runs must pump the channel.

## Consequences

- The observation half proves the *shipped* engine boots, hunts, and changes observable state
  — evidence about the real binary, which is the whole point of §3.
- Stated so it is not assumed: verify steps that need injected stimulus prove behaviour on a
  `SAPPER_TEST_HOOKS` binary, not the shipped one. The unflagged observation half is what
  keeps "the shipped engine runs" honest; the flagged half only sets up deterministic
  conditions the shipped code then executes.
- No release binary carries a command that transmits — deauth and upload cannot be triggered
  over serial against an arbitrary target, because that vocabulary does not exist outside
  `SAPPER_TEST_HOOKS`.
- This ADR creates standing invariants added to `CLAUDE.md` §4 in the same commit that
  accepts it: (a) observation commands mutate no engine state; (b) stimulus injection enters
  the engine only through the seams real events use; (c) every pre-engine blocking state
  pumps the channel; (d) actuation vocabulary is absent from non-`SAPPER_TEST_HOOKS` builds.
