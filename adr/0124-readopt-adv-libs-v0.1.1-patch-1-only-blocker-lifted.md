---
id: '0124'
title: "Re-adopt esp32-arduino-adv-libs v0.1.1 (patch #1 only): the ADR-0123 blocker is lifted"
type: architecture
status: accepted
date: 2026-11-15
supersedes: ['0123']
superseded_by: []
---

## Context

ADR-0123 reverted the v0.1.0 adoption (patch #2 corrupts the esp32 core's `.ext_ram.bss`) and put `main`
back on the Bruce blob with the local deauth-weaken hook restored. Its decision 3 **blocked re-adoption** on
an upstream fix (withdraw patch #2 or make it safe) and named the recommended action: withdraw patch #2,
since its only intended use (static `EXT_RAM_BSS_ATTR` placement) was independently shown infeasible for this
firmware, so it carried no benefit and one real harm. Decision 4 made on-device boot of each affected chip
family a **required pre-merge gate** for any future libs adoption, because the v0.1.0 swap passed the link
and the native suite yet bricked every esp32 board.

**The blocker is lifted (measured upstream, 2026-11-15).** `esp32-arduino-adv-libs` withdrew patch #2
(`esp32-adv-libs:ADR-0028`, supersedes `esp32-adv-libs:ADR-0021`) and cut **v0.1.1** carrying patch #1 alone
(asset `adv-esp32-arduino-libs-v5.5-20261115-e68e301.zip`). The upstream evidence this re-adoption rests on:

- **Config surface, patch #2 gone.** The v0.1.1 esp32 base `sdkconfig` reads
  `# CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is not set`, and neither esp32 variant header defines the
  symbol, while `CONFIG_ESP32_REV_MIN_3=y` (the IRAM-parity append) is kept. So the invalid sdkconfig
  combination that corrupted `.ext_ram.bss` (patch #2's `ALLOW_BSS=y` with PSRAM boot-init off,
  `esp32-adv-libs:ADR-0015`) no longer exists on any surface a consumer compiles against.
- **Patch #1 intact.** `ieee80211_raw_frame_sanity_check` still ships **weak** on both chips, asserted in the
  release build by `esp32-adv-libs:scripts/0020-verify.mjs` (the upstream standing invariant 11). This is the
  property that retires our local weaken hook.
- **Boot-verified upstream before the tag.** v0.1.1 was flashed and booted clean on an esp32 board
  (`m5stickc_plus2`, `heap_max=2064372`, the exact board v0.1.0 crash-looped with `EXCVADDR=0xaaaaa9dc`) and
  an esp32s3 board (`t_embed`, `heap_max=8257524`), with zero crash markers, satisfying the upstream
  `esp32-adv-libs:ADR-0028` decision 4 gate.

## Decision

1. **Re-adopt v0.1.1 on all five envs** (`cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`,
   `atom_echo`): each env's `framework-arduinoespressif32-libs` points at the v0.1.1 asset instead of the
   Bruce blob, and the local deauth-weaken hook retires again (`scripts/weaken_deauth_pre.py`,
   `scripts/weaken_deauth_symbol.sh`, and the `extra_scripts` wiring in `[env_embedded]` and `[testhooks]`),
   exactly as slice-0122 intended. The strong `ieee80211_raw_frame_sanity_check` override in
   `raw_transmitter_esp32.cpp` links against the libs' weak symbol with no local `objcopy`. This reverses
   ADR-0123 decision 1 (restore the Bruce blob + hook): that state was a correct response to a broken release,
   not a wrong analysis, and it is reversed now only because the precondition ADR-0123 itself set (decision 3)
   is met. ADR-0123's root-cause finding remains true and is preserved in its immutable body.

2. **The pre-merge on-device boot gate (ADR-0123 decision 4) is honoured, not deferred.** This re-adoption
   flashes and boots at least one esp32 board and one esp32s3 board **before** the merge to `main`, and
   records the boot evidence in slice-0125's `## As built`. This is the first adoption to run under the gate
   the v0.1.0 incident created; the whole point of that lesson was to make the next swap prove itself on
   hardware before it ships, not after.

3. **Re-establish standing invariant #52:** the weak `ieee80211_raw_frame_sanity_check` is a **pinned-artifact
   property of the owned libs** (patch #1, attested upstream by `esp32-adv-libs:ADR-0019` /
   `esp32-adv-libs:scripts/0020-verify.mjs`), so no local weaken hook ships and the raw-TX override links
   against the libs' weak symbol directly. The number #52 is the one ADR-0121 created and ADR-0123 removed; it
   is reused, not a new number, because this is the same invariant returning and nothing was created above it
   in the interim (the table topped out at #51 after the revert).

4. **Placement stays on the runtime route, permanently.** The three-way manifest placement
   (`BOARD_HAS_PSRAM` runtime `MALLOC_CAP_SPIRAM` / internal heap / static `.bss`, ADR-0080) is untouched.
   Patch #2 is gone upstream, so the static `EXT_RAM_BSS_ATTR` supersession of ADR-0080 decision 5 is now
   **permanently off the table**, not merely deferred: `esp32-adv-libs:ADR-0028`'s reopen condition requires
   PSRAM valid at boot, which this firmware's boot-init-off config (the PSRAM-less-reference-board safety,
   `esp32-adv-libs:ADR-0015`) deliberately does not provide. ADR-0123 already corrected ADR-0080 decision 5's
   assumption; this closes it: the runtime `MALLOC_CAP_SPIRAM` route is the end-state mechanism on esp32 PSRAM
   boards, and no static-attribute follow-up is coming.

## Consequences

- `main` returns to the owned `esp32-arduino-adv-libs` build on all five boards, now on a release that cannot
  reproduce the esp32 crash loop (patch #2 is absent from every config surface); the weaken hook retires and
  the two scripts are deleted again.
- ADR-0123 carries `superseded_by: ['0124']`. The supersession chain for "which libs + hook state" is
  ADR-0121 (adopt v0.1.0) → ADR-0123 (revert) → ADR-0124 (re-adopt v0.1.1), each the current decision in
  turn; ADR-0123's measured root cause and its pre-merge-boot lesson live on in its body.
- §4 invariant #52 returns, sourced by this ADR.
- The LEDGER's shared-libs feature line is closed (folded into slice-0125): the Sapper has retired the Bruce
  blob for the owned build, which was the deliverable.
- The sibling Adversary repo, which never adopted v0.1.0, can now adopt v0.1.1 directly and retire its own
  weaken hook, under the same pre-merge boot gate; its LEDGER mirror is updated to point at v0.1.1 with the
  blocker lifted.
