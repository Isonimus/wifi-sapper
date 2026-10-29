---
id: '0101'
title: "Decline a log-level gate; the release ships the normal envs"
type: architecture
status: accepted
date: 2026-10-29
supersedes: []
superseded_by: []
---

## Context

The pre-release program carried a planned item (LEDGER, "reduced-logging release env"): add a
`SAPPER_LOG_LEVEL`-style compile gate and per-board `*_release` envs, then repoint the release
matrix (ADR-0095) at them. The rationale was explicitly carried over from the parent Adversary,
which ships a `cardputer_release` env at WARN level and recovers about 17 KB by compiling
below-threshold log macros to `do {} while (0)` so their format strings never reach the image
(adversary:ADR-0051). This ADR records the measurement that was taken before building it, and the
decision that measurement forced.

**Finding 1: there is no flash pressure to relieve.** The app image is ~1.4 MB on every board,
against 3 MB-class app partitions. Measured on the current `main` builds (2026-10-29):

| Board | Partition | App slot | `firmware.bin` | Used | Free |
|---|---|---|---|---|---|
| CYD (ESP32-3248S035C, classic, 4 MB, no OTA) | `huge_app.csv` | 3,145,728 B | 1,407,232 B | 44.7% | 1,738,496 B (~1.66 MB) |
| M5StickC Plus2 (OTA) | `default_8MB.csv` | 3,342,336 B (x2) | 1,420,256 B | 42.5% | 1,922,080 B (~1.83 MB) per OTA slot |
| Cardputer / T-embed (ESP32-S3) | `huge_app`-class | >= 3,145,728 B | ~1.40 MB | < 47% | ~1.7 MB+ |

The CYD is the tightest case (smallest total flash, no OTA second slot) and is still 55% empty.

**Finding 2: the strippable surface is tiny.** Of 226 `Serial.print*` call sites, 117 are
`[FATAL]`/`[ERROR]` (always-keep, fail-loud per the quality bar). The remaining ~109 sites hold
about 6.5 KB of format-string literals. A full strip recovers the low tens of KB at most (rodata
plus some shared call-site code), which is under 1% of the free space on the worst board. The
parent's 17 KB win was material against its budget; here it moves nothing.

**Finding 3: the parent's `log.h` mis-fits by construction.** Its model *is* severity: the macros
emit `[ERROR]`/`[WARN]`/`[INFO]`/`[DEBUG]` as the line prefix and strip by level. The Sapper's
markers are **domains**, not severities: `[SYNC]`, `[CAPTURE]`, `[UPLOAD]`, `[HUNT]`, `[DEAUTH]`,
`[BATT]`, `[DIM]`, and the like. Those domain markers are load-bearing twice over: they are the
verify contract (`scripts/0020-sync-verify.mjs` greps `^\[SYNC\] verify sync ok`,
`scripts/0048-serial-stimulus-verify.mjs` greps `^\[UPLOAD\]`, and so on across the verify suite),
and they are the diagnosis surface for a headless-first appliance whose boards may have no screen.
Porting severity tiers would mean re-tagging ~109 sites and putting every verify regex at risk, to
win the non-existent flash above.

**Finding 4: there is nothing for a `*_release` env to differ in.** The normal `cardputer` /
`m5stickc_plus2` / `t_embed` / `cyd_3248s035c` envs already carry no `SAPPER_TEST_HOOKS` vocabulary
(that lives only in the `*_testhooks` siblings) and are exactly what `release.yml` builds today
(ADR-0095). A `*_release` env would be a near-duplicate of the normal env, differing only in a
logging knob that findings 1-3 say not to add.

## Decision

Do **not** add a `SAPPER_LOG_LEVEL`/severity log-level gate, and do **not** add per-board
`*_release` envs. `release.yml` keeps building the normal, testhooks-free, domain-markered envs
(ADR-0095). The parent's `log.h` severity-tier layer (adversary:ADR-0051) is **not** ported.

This decision is conditional on the measured facts above. It must be revisited if, and only if,
flash becomes tight: a much larger feature set that pushes an image past its partition, or enabling
OTA on a 4 MB board (which would split the app region into two ~1.5 MB slots). At that point the
right mechanism is still unlikely to be a wholesale severity re-tag: a targeted strip of the
specific verbose lines, keeping every domain marker the verify suite and field diagnosis depend on,
would recover most of the little there is to recover without touching the contract.

A single future verbose line that genuinely must be present on a bench build but absent from the
shipped/release image (the shipped drain-outcome summary line that ADR-0021's "drain facts stay off
the sync-only logger" decision currently withholds, and that the `verify:serial-stimulus` gap in the
LEDGER needs) is gated with **one targeted compile flag at that one site** when it lands, not by
introducing a tiering system ahead of its first real customer.

## Consequences

- The release images stay byte-identical to the verified bench builds in everything but nothing:
  there is no release/bench logging divergence to reason about, so the binary an operator flashes
  via the future web flasher (D2) is the binary the verify scripts exercised.
- A deployed headless unit keeps its full domain-marker serial telemetry, so plugging in USB to
  diagnose a misbehaving field unit shows what it is doing, not just its errors. For an autonomous
  appliance this is a feature, not noise, and it is the reason the aggressive parent-style strip was
  the wrong default here even before the flash numbers removed its payoff.
- No `Serial.print*` site is migrated, so no verify regex is put at risk and the native suite is
  untouched. The ~226 sites stay raw `Serial.print*`, which is the KISS floor until a measured need
  says otherwise.
- The LEDGER item is rewritten from a planned feature to a deferred-with-condition entry citing this
  ADR, so the measurement is not re-derived and the "why we did not" is not re-litigated.
- This does not gate, bless, or block the shipped drain-summary line itself: that remains an
  ADR-0021-reversing decision to be taken on its own merits (LEDGER). This ADR only settles that
  *if* that line wants to be bench-only, a single compile flag is the mechanism, not a log-level
  system.
