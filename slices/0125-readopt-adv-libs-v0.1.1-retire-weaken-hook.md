---
id: '0125'
title: "Swap all envs to esp32-arduino-adv-libs v0.1.1 and delete the deauth-weaken hook (re-adoption)"
type: slice
status: accepted
date: 2026-11-15
supersedes: []
superseded_by: []
---

## Goal

Execute ADR-0124: re-adopt the owned `esp32-arduino-adv-libs` build, now on **v0.1.1** (patch #1 only), on
every env and delete the local deauth-weaken hook again, which v0.1.1's weak
`ieee80211_raw_frame_sanity_check` (baked in-build, attested upstream by `esp32-adv-libs:ADR-0019`) makes
redundant. This re-does what slice-0122 shipped and ADR-0123 reverted: the v0.1.0 adoption bricked every
esp32 board because patch #2 (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`) corrupted the Arduino core's
unzeroed `.ext_ram.bss`; `esp32-adv-libs` withdrew patch #2 (`esp32-adv-libs:ADR-0028`) and cut v0.1.1
carrying patch #1 alone, so the blocker ADR-0123 named is lifted. This closes the LEDGER shared-libs feature
line (own the shared build, retire the Bruce blob): the deliverable is the Sapper pinning the owned build with
the hook gone. No new behaviour ships: the raw-TX override and the runtime deauth arm (ADR-0027/0029) are
unchanged, placement is untouched (ADR-0080's runtime `MALLOC_CAP_SPIRAM` route stays, permanently, ADR-0124
decision 4); only where the weak symbol comes from changes (a pinned-artifact property, not a per-build
`objcopy`). Unlike slice-0122, the esp32 and esp32s3 on-device boot is a **pre-merge** acceptance scenario,
not a post-merge follow-up, as ADR-0123 decision 4 now requires.

## Definition of Done

- **Given** all five shipping envs (`cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`, `atom_echo`)
  pointed at the v0.1.1 `framework-arduinoespressif32-libs` asset and the weaken hook removed
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

- **Given** the re-adoption firmware (v0.1.1, hook removed), flashed **before** the merge to `main`
- **When** one esp32 board (`m5stickc_plus2`, the board v0.1.0 crash-looped) and one esp32s3 board (`t_embed`)
  are flashed and booted
- **Then** each boots clean with no `LoadProhibited` reboot loop and no `[FATAL]`, reaching a live hunt-loop
  `[STATE]` phase, confirming the esp32 `.ext_ram.bss` corruption is gone and the esp32s3 octal PSRAM still
  inits via late `psramInit()` (ADR-0124 decision 2, the gate ADR-0123 decision 4 requires before merge)

## Design

Measured basis and the decision are in ADR-0124. What this slice changes, nothing more. It is the forward
version of the ADR-0123 revert, with the v0.1.0 URL replaced by v0.1.1 and the comments corrected for
patch #2's absence:

- **The five URLs.** Each env's `platform_packages` line
  `framework-arduinoespressif32-libs @ <bruce-blob-url>` becomes the v0.1.1 asset
  `https://github.com/Isonimus/esp32-arduino-adv-libs/releases/download/v0.1.1/adv-esp32-arduino-libs-v5.5-20261115-e68e301.zip`,
  and the adjacent comment is corrected to name the owned build v0.1.1 and what it provides: weak
  `ieee80211_raw_frame_sanity_check` on both chips (patch #1). It must **not** claim the esp32 BSS flag, which
  v0.1.1 withdrew (the false claim slice-0122 carried and that bricked the esp32 boards).
- **The hook, deleted.** `scripts/weaken_deauth_pre.py` and `scripts/weaken_deauth_symbol.sh` are removed, the
  `extra_scripts = pre:scripts/weaken_deauth_pre.py` line and its comment block leave `[env_embedded]`, and
  the `pre:scripts/weaken_deauth_pre.py` line leaves `[testhooks]`'s `extra_scripts` (the pcap embedder
  `pre:scripts/embed_test_pcap.py` stays; its re-listing comment is updated since there is no longer a weaken
  hook to re-list). The `raw_transmitter_esp32.cpp` comment is updated to state the symbol ships weak in the
  pinned libs (§4 #52), not that a local pre-script ran.
- **Placement code: untouched.** The three-way branch at `hunt_loop.cpp` and `main.cpp`
  (`BOARD_HAS_PSRAM` / `SAPPER_MANIFEST_HEAP_INTERNAL` / static `.bss`) is not edited. T-embed and the
  M5StickC Plus2 keep the runtime `MALLOC_CAP_SPIRAM` route (ADR-0080); the static `EXT_RAM_BSS_ATTR` form is
  permanently off the table (ADR-0124 decision 4), so there is no placement follow-up this time.
- **§4 invariant #52, re-established** in `CLAUDE.md`, sourced by ADR-0124 (same number ADR-0121 created and
  ADR-0123 removed).

**Existing code considered.** This slice re-applies the slice-0122 adoption (reverted by the ADR-0123 commit
`79096ee`) rather than inventing a new mechanism: the diff is the inverse of the revert, with the URL bumped
to v0.1.1 and the patch-#2 claims dropped from the comments. The weaken hook is the only `extra_scripts` entry
in `[env_embedded]`, so removing it empties that key; `[testhooks]` keeps the independent pcap embedder. No
source module imports the hook; the single reference is a comment in `raw_transmitter_esp32.cpp`. No pure
logic, no test, and no placement code is touched, so the native suite is unchanged by construction. A
sapper-side `scripts/*-verify.mjs` is deliberately not added (see Verification).

## Verification

- **Per-env build/link** (the machine check, Definition-of-Done scenarios 1 and 2): each of the five envs is
  built against v0.1.1 with the hook removed. The link succeeding without a "multiple definition" error is the
  proof the libs' weak symbol resolves the override; no `.bss` overflow is the proof no budget regressed.
  Recorded in `## As built`. There is no new `scripts/*-verify.mjs`: the link is the assertion, and the
  weak-symbol property is already gated upstream (`esp32-adv-libs:scripts/0020-verify.mjs`), so a sapper-side
  verify script would duplicate an upstream check and run nowhere useful (the same reasoning slice-0122 gave).
- **Native suite** (Definition-of-Done scenario 3): `pio test -e native` passes unchanged (no pure module
  touched). Recorded in `## As built`.
- **On-device boot before merge** (Definition-of-Done scenario 4, ADR-0124 decision 2 / ADR-0123 decision 4):
  the re-adoption firmware is flashed to `m5stickc_plus2` (esp32, the board v0.1.0 bricked) and `t_embed`
  (esp32s3) and each is booted; the boot serial shows a clean `[STATE]` line with no `LoadProhibited` loop and
  no `[FATAL]`. This is the gate the link cannot assert and whose deferral let v0.1.0 ship broken; this time it
  runs before the merge, and the merge does not happen until it passes. Recorded in `## As built`.

## As built

Filled in at merge, in past tense, confirming each Definition-of-Done scenario was met or recording the
deviation. The per-env build/link results and the native-suite result are recorded in an amendment below once
green; the esp32 and esp32s3 boot proofs are recorded when they pass, and the merge to `main` happens only
after scenario 4 passes.

## Amendment: 2026-11-15 Re-adopted on all five envs; esp32 and esp32s3 boot-verified before the merge

Shipped as designed. All five shipping envs build and link against `esp32-arduino-adv-libs` **v0.1.1**
(resolved `framework-arduinoespressif32-libs@5.5.1+sha.8410210c9a0`, fetched from the Isonimus v0.1.1 release)
with the weaken hook removed.

**Definition-of-Done scenario 1 (build/link), met.** `pio run` on all five envs succeeded with **no
"multiple definition" of `ieee80211_raw_frame_sanity_check`** (the libs' weak symbol resolves the override)
and **no `.dram0.bss` / region overflow** (no budget regressed): `cardputer` (RAM 49.1%, Flash 42.0%),
`m5stickc_plus2` (31.9% / 42.5%), `t_embed` (31.5% / 21.4%), `cyd_3248s035c` (31.8% / 44.8%), `atom_echo`
(31.3% / 42.6%).

**Definition-of-Done scenario 2 (hook retired), met.** `scripts/weaken_deauth_pre.py` and
`scripts/weaken_deauth_symbol.sh` are deleted (`git rm`), the `extra_scripts` weaken line is gone from
`[env_embedded]` and `[testhooks]` (the pcap embedder stays), and a tree search over `.ini`/`.py`/`.sh`/
`.cpp`/`.h` finds no `weaken_deauth` reference. The `raw_transmitter_esp32.cpp` comment now states the symbol
ships weak in the pinned libs (§4 #52).

**Definition-of-Done scenario 3 (native suite unchanged), met.** `pio test -e native` passed **503/503**
unchanged (no pure module touched).

**Definition-of-Done scenario 4 (on-device boot before merge), met.** Each board was flashed with this
slice's firmware (`pio run -e <env> -t upload`) and its boot serial captured. Both booted clean, **before**
the merge, as ADR-0124 decision 2 / ADR-0123 decision 4 require:

- **esp32, M5StickC Plus2** (the board `v0.1.0` crash-looped with `EXCVADDR=0xaaaaa9dc`, ~7 reboots in 12 s):
  a single boot, no reboot loop, zero crash markers (no `Guru Meditation` / `LoadProhibited` / backtrace)
  across the capture window, reaching `[STATE] phase=provisioning heap_free=161496 heap_max=2064372
  disp=240x135` then the captive portal (`[PORTAL] ssid=Sapper-DE49`), with 2 MB PSRAM on the heap
  (`psramAddToHeap`) and the display live. The `.ext_ram.bss` corruption that patch #2 caused is gone. The
  `nvs_get_str ... NOT_FOUND` lines are the expected fresh-NVS portal route, not a fault.
- **esp32s3, T-embed**: a single `USB_UART_CHIP_RESET`, zero crash markers, reaching `[STATE]
  phase=station_connect heap_free=140900 heap_max=8257524 disp=320x170` with the 8 MB octal PSRAM up via late
  `psramInit()` (the owned esp32s3 libs ship boot-init off, `esp32-adv-libs:ADR-0015`) and the display live.
  The repeated `NO_AP_FOUND` is the bench having no matching uplink, not a fault.
- **esp32, Atom Echo** (no PSRAM, headless, the internal-heap manifest path `SAPPER_MANIFEST_HEAP_INTERNAL`
  that the M5StickC Plus2 does not exercise): a single `POWERON_RESET`, zero crash markers, reaching `[STATE]
  phase=ready heap_free=186312 heap_max=110580 disp=0x0` (`heap_max=110580` confirms no PSRAM, matching the
  cyd figure in ADR-0123; `disp=0x0` is the NullDisplay) and running on into the live hunt loop (`[HUNT]
  discovering channels=1-13`, `[DEAUTH] ARMED`, `[DEAUTH] txOk=0`). This was an optional extra beyond the
  one-esp32-plus-one-esp32s3 gate, taken to also cover the no-PSRAM classic-esp32 boot path on hardware.

Both heap figures match the upstream v0.1.1 boot proof (`esp32-adv-libs` slice-0029: `heap_max=2064372` on
the same M5StickC Plus2, `heap_max=8257524` on the same T-embed), so the re-adopted libs behave identically
on hardware to the upstream boot-tested artifact. Scenarios 1-4 all met; the merge to `main` follows this
proof, not the other way round.
