---
id: '0122'
title: "Swap all envs to esp32-arduino-adv-libs v0.1.0 and delete the deauth-weaken hook"
type: slice
status: accepted
date: 2026-11-15
supersedes: []
superseded_by: []
---

## Goal

Execute the first step of ADR-0121: point every env's `framework-arduinoespressif32-libs` at the owned
`esp32-arduino-adv-libs` v0.1.0 release and delete the local deauth-weaken hook, which v0.1.0's patch #1
(weak `ieee80211_raw_frame_sanity_check`, baked in-build and verified upstream by
`esp32-adv-libs:ADR-0019`) makes redundant. This is the half of the LEDGER shared-libs feature that retires
a downstream hook; it leaves manifest placement untouched, so the `EXT_RAM_BSS_ATTR` supersession of
ADR-0080 (esp32-only, ADR-0121 decision 3) stays a tracked follow-up. No new behaviour ships: the raw-TX
override and the runtime deauth arm (ADR-0027/0029) are unchanged; only where the weak symbol comes from
changes (a pinned artifact property, not a per-build `objcopy`).

## Definition of Done

- **Given** all five shipping envs (`cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`, `atom_echo`)
  pointed at the v0.1.0 `framework-arduinoespressif32-libs` asset and the weaken hook removed
- **When** each env is built with PlatformIO
- **Then** it compiles and **links** (the strong `ieee80211_raw_frame_sanity_check` override in
  `raw_transmitter_esp32.cpp` resolves against the libs' weak symbol with no "multiple definition" error),
  with no `.dram0.bss` overflow regression (an over-budget `.bss` is a hard link failure)

- **Given** the hook is retired
- **When** the tree is searched for `weaken_deauth_pre` / `weaken_deauth_symbol`
- **Then** nothing references them: both scripts are deleted and the `extra_scripts` lines are gone from
  `[env_embedded]` and `[testhooks]` (the mechanism is fully removed, not half-wired)

- **Given** the native test suite (placement and all pure modules untouched by this slice)
- **When** `pio test -e native` runs
- **Then** it passes unchanged, confirming the swap is a dependency/build change with no logic impact

## Design

Measured basis and the decision are in ADR-0121. What this slice changes, nothing more:

- **The five URLs.** Each env's `platform_packages` line
  `framework-arduinoespressif32-libs @ <bruce-blob-url>` becomes the v0.1.0 asset
  `https://github.com/Isonimus/esp32-arduino-adv-libs/releases/download/v0.1.0/adv-esp32-arduino-libs-v5.5-20261115-e68e301.zip`,
  and the adjacent "Bruce's patched ESP32 Arduino libs" comment is corrected to name the owned build and
  what it provides (weak `ieee80211_raw_frame_sanity_check` on both chips; the esp32 BSS flag).
- **The hook, deleted.** `scripts/weaken_deauth_pre.py` and `scripts/weaken_deauth_symbol.sh` are removed,
  the `extra_scripts = pre:scripts/weaken_deauth_pre.py` line and its comment block leave `[env_embedded]`,
  and the `pre:scripts/weaken_deauth_pre.py` line leaves `[testhooks]`'s `extra_scripts` (the pcap embedder
  `pre:scripts/embed_test_pcap.py` stays; its re-listing comment is updated since there is no longer a
  weaken hook to re-list). The `raw_transmitter_esp32.cpp` comment that said "the weaken_deauth_pre.py
  pre-script already ran for the shipped env" is updated to state the symbol ships weak in the pinned libs.
- **Placement code: untouched.** The three-way branch at `hunt_loop.cpp` and `main.cpp`
  (`BOARD_HAS_PSRAM` / `SAPPER_MANIFEST_HEAP_INTERNAL` / static `.bss`) is not edited. T-embed and the
  M5StickC Plus2 keep the runtime `MALLOC_CAP_SPIRAM` route (ADR-0080); the esp32-only `EXT_RAM_BSS_ATTR`
  supersession is the follow-up (ADR-0121 decision 3, LEDGER).

**Existing code considered.** The weaken hook is the only `extra_scripts` entry in `[env_embedded]`, so
removing it empties that key (no other pre-action depends on it); `[testhooks]` keeps the independent pcap
embedder. No source module imports the hook; the single reference is a comment in `raw_transmitter_esp32.cpp`.
No pure logic, no test, and no placement code is touched, so the native suite is unchanged by construction.

## Verification

- **Per-env build/link** (the machine check, Definition-of-Done scenarios 1 and 2): each of the five envs is
  built against v0.1.0 with the hook removed. The link succeeding without a "multiple definition" error is
  the proof the libs' weak symbol resolves the override; no `.bss` overflow is the proof no budget regressed.
  Recorded in `## As built`. There is no new `scripts/*-verify.mjs`: the link is the assertion, and the
  weak-symbol property is already gated upstream (`esp32-adv-libs:scripts/0020-verify.mjs`), so a sapper-side
  verify script would duplicate an upstream check and run nowhere useful.
- **Native suite** (Definition-of-Done scenario 3): `pio test -e native` passes unchanged (no pure module
  touched). Recorded in `## As built`.
- **T-embed on-device PSRAM proof** (ADR-0121 decision 5, operator's to run, not assertable here): on a
  flashed T-embed, `psramFound()` is true and the cracked manifest's `MALLOC_CAP_SPIRAM` allocation is
  non-null (the ADR-0080 `[FATAL]` guard stays silent), confirming 8 MB octal PSRAM still initialises via
  late `psramInit()` under the owned esp32s3 libs' boot-init-off config. Tracked in the LEDGER; a failure
  moves T-embed's esp32s3-PSRAM manifest to static `.bss` in a follow-up.

## As built

Filled in at merge, in past tense, confirming each Definition-of-Done scenario was met or recording the
deviation. The per-env build/link results and the native-suite result are recorded in an amendment below at
merge; the T-embed on-device PSRAM proof is recorded when the operator runs it.

## Amendment: 2026-11-15 Built and linked on all five envs; sdkconfig flags confirmed

Shipped as designed. All five shipping envs built and linked against `esp32-arduino-adv-libs` v0.1.0 with
the weaken hook removed (`pio run`, local toolchain): `cardputer` (RAM 49.1%, both manifests static `.bss`),
`m5stickc_plus2` (27.1%, manifests on the PSRAM heap), `t_embed` (31.5%), `cyd_3248s035c` (27.0%, internal
heap), `atom_echo` (26.5%, internal heap). Every link succeeded with **no "multiple definition" of
`ieee80211_raw_frame_sanity_check`** (Definition-of-Done scenario 1: the libs' weak symbol resolves the
override) and **no `.dram0.bss` overflow** (no budget regressed). A tree search finds no `weaken_deauth_pre`
/ `weaken_deauth_symbol` reference and both scripts are deleted (scenario 2). `pio test -e native` passed
**503/503** unchanged (scenario 3).

The resolved sdkconfig the build pulled confirms the mechanism, measured not assumed: **esp32** ships
`CONFIG_SPIRAM_USE_MALLOC=y` (so `m5stickc_plus2`'s runtime `MALLOC_CAP_SPIRAM` route still allocates) and
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` (patch #2, so Change 2's static attribute will link on
esp32); **esp32s3** ships `CONFIG_SPIRAM_USE_MALLOC=y` but leaves `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`
unset (patch #2 is esp32-only, ADR-0121 decision 3 confirmed). Both chips leave `CONFIG_SPIRAM_BOOT_HW_INIT`
unset, so PSRAM comes up via Arduino's late `psramInit()` on `BOARD_HAS_PSRAM`; that path is already proven
on esp32 (`m5stickc_plus2`), but the 8 MB octal PSRAM on `t_embed` under the owned esp32s3 libs is the open
on-device question (ADR-0121 decision 5, LEDGER) the link cannot assert.

Definition-of-Done scenarios 1-3 are met on the machine-checkable half; the `t_embed` on-device PSRAM proof
remains the operator's to run.
