---
id: '0121'
title: "Adopt the owned esp32-arduino-adv-libs build (v0.1.0) and retire the local deauth-weaken hook"
type: architecture
status: accepted
date: 2026-11-15
supersedes: []
superseded_by: []
---

## Context

This repo pins `framework-arduinoespressif32-libs` to a third-party precompiled blob (bmorcelli's
`esp32-arduino-lib-builder` release, the "Bruce" libs) and layers a local pre-build hook on top of it:
`scripts/weaken_deauth_pre.py` runs `scripts/weaken_deauth_symbol.sh` (an `objcopy --weaken-symbol` over
the precompiled `libnet80211.a`) on every build, so the strong `ieee80211_raw_frame_sanity_check` override
in `raw_transmitter_esp32.cpp` (the raw management-frame TX seam, ADR-0027, shipped in every device build
since ADR-0029) links without a "multiple definition" error. The hook exists only because the blob ships
that symbol **strong** and we do not own the build.

The shared patched-libs initiative tracked in the LEDGER has now shipped. `Isonimus/esp32-arduino-adv-libs`
cut **v0.1.0**, a drop-in replacement for the Bruce blob (identical `framework-arduinoespressif32-libs/<chip>/...`
layout) that bakes the two patches this repo and the sibling Adversary were carrying as downstream hooks:

- **Patch #1**: `libnet80211.a` ships with `ieee80211_raw_frame_sanity_check` **weak**, in-build, enforced
  upstream by `esp32-adv-libs:ADR-0019` (its invariant 11, `nm`-checked by `scripts/0020-verify.mjs` on
  every release). This is exactly what the local weaken hook was faking downstream, now a property of the
  pinned artifact.
- **Patch #2**: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, which makes static `EXT_RAM_BSS_ATTR`
  placement link. It is set on **esp32 only** (`esp32-adv-libs:ADR-0021`, its invariant 12), because the
  shared build ships esp32s3 with PSRAM boot-init **off** (`esp32-adv-libs:ADR-0015`, invariant 9) so its
  PSRAM-less reference board does not boot-loop.

v0.1.0's provenance is itself guarded upstream by a resolved-dependency drift gate (`esp32-adv-libs:ADR-0026`).
ADR-0080 decision 5 named this moment in advance: "when the shared patched-libs initiative lets us own the
IDF build and set `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, `EXT_RAM_BSS_ATTR` becomes a one-line
static attribute ... at that point this ADR is superseded by the attribute form."

Two facts from the upstream invariants shape how far this adoption can go **now**:

1. The weak-symbol patch (#1) applies to **both** chips, so the weaken hook can retire for the whole board
   matrix the moment every env points at v0.1.0.
2. The BSS-flag patch (#2) applies to **esp32 only**, and esp32s3 ships PSRAM boot-init off. So the
   `EXT_RAM_BSS_ATTR` supersession of ADR-0080 is available on the esp32 PSRAM board (M5StickC Plus2) but
   **not** on the esp32s3 PSRAM board (T-embed), and T-embed's swap carries an unverified question: whether
   its 8 MB octal PSRAM still comes up via Arduino's late `psramInit()` when the libs disable boot-init.
   T-embed was never in the shared build's test matrix (its esp32s3 reference board is PSRAM-less).

## Decision

1. **Point every env's `framework-arduinoespressif32-libs` at `esp32-arduino-adv-libs` v0.1.0** (all five
   shipping envs: `cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`, `atom_echo`; the `*_testhooks`
   envs extend them). This retires the dependency on a third-party maintainer's blob and gives the appliance
   an auditable, owned RF-stack provenance (the LEDGER feature's payoff #3).

2. **Delete the local deauth-weaken hook** (`scripts/weaken_deauth_pre.py`, `scripts/weaken_deauth_symbol.sh`,
   and the `extra_scripts = pre:scripts/weaken_deauth_pre.py` wiring in `[env_embedded]` and `[testhooks]`).
   The weak `ieee80211_raw_frame_sanity_check` is now a property of the pinned libs (patch #1, verified
   upstream), so the downstream `objcopy` is redundant: `raw_transmitter_esp32.cpp`'s strong override links
   against the libs' weak symbol directly. A local framework-symbol rewrite hook is **not** to be re-added;
   if a future libs bump were to ship the symbol strong, that is an upstream regression the shared build's
   own gate (`esp32-adv-libs:scripts/0020-verify.mjs`) must catch, not a thing this repo papers over again.

3. **Placement is unchanged by this ADR; T-embed keeps ADR-0080's runtime PSRAM route.** Because patch #2 is
   esp32-only and esp32s3 ships PSRAM boot-init off, the `EXT_RAM_BSS_ATTR` static supersession ADR-0080
   anticipated is available **only** for the esp32 PSRAM board, and is deferred to a follow-up (LEDGER), not
   taken here. Every board keeps the manifest-placement route it ships today: M5StickC Plus2 and T-embed the
   runtime `MALLOC_CAP_SPIRAM` heap route (ADR-0080), the CYD and Atom Echo the internal-heap route
   (ADR-0082), the Cardputer static `.bss`. This keeps the adoption's diff to the dependency + the hook, so a
   link regression is attributable to the swap alone, not entangled with a placement rewrite.

4. **The adoption's correctness is machine-checked by the link, per board.** Each env building and linking
   against v0.1.0 without the hook is the proof that the libs' weak symbol resolves the override and that no
   `.bss` budget regressed (an over-budget `.dram0.bss` is a hard build failure, the same machine check
   §4 #36/#37 rely on). This is asserted on every build of every env, not once.

5. **T-embed's PSRAM boot under the owned esp32s3 libs is an on-device proof, the operator's to run.** The
   link cannot assert that 8 MB octal PSRAM still initialises via late `psramInit()` when the libs disable
   boot-init. Until that is confirmed on hardware (`psramFound()` true and the cracked manifest's
   `MALLOC_CAP_SPIRAM` allocation non-null, the ADR-0080 `[FATAL]` guard silent), T-embed's swap is
   provisional; a boot that fails the guard means the esp32s3-PSRAM manifest falls back to static `.bss`
   (the S3 has the DRAM room, as the Cardputer proves) in a follow-up. Tracked in the LEDGER.

## Consequences

- The build no longer runs a per-build `objcopy` over a vendored archive; the weak symbol is an attested
  property of the pinned release instead of a downstream rewrite. One fewer moving part on every build, and
  the "a libs reinstall silently restores the strong symbol and breaks the link" failure the hook guarded
  against is now impossible by construction (the symbol ships weak in the artifact).
- A new §4 standing invariant (#52) records that the weak `ieee80211_raw_frame_sanity_check` is provided by
  the pinned adv-libs build, so a future reader does not re-introduce a local weaken hook or wonder where the
  symbol's weakness comes from.
- The `EXT_RAM_BSS_ATTR` supersession of ADR-0080 remains open, now **scoped to esp32** by the measured
  esp32-only reach of patch #2. A LEDGER item carries it with this ADR cited, so the follow-up has the
  constraint recorded rather than re-deriving why T-embed is excluded.
- T-embed ships on the owned libs provisionally pending the on-device PSRAM proof; the LEDGER tracks it
  alongside the esp32-only supersession.
- The sibling Adversary repo retires its own copy of the weaken hook separately against the same v0.1.0
  release (its LEDGER carries the mirror); this ADR governs only this repo.
