---
id: '0082'
title: "Large-manifest placement on a no-PSRAM classic ESP32: one-at-a-time internal DRAM heap"
type: architecture
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Context

slice-0083 brings up the ESP32-3248S035C ("CYD"), the fourth board and the first that is **both no-PSRAM
and a classic ESP32** (ESP32-D0WD-V3, 4 MB flash). The three prior boards each had an escape the CYD does
not: the reference Cardputer is no-PSRAM but a roomy S3; the M5StickC Plus2 and T-embed are classic/PSRAM
and S3/PSRAM respectively. ADR-0080 set the manifest-placement rule as a binary on `BOARD_HAS_PSRAM`:
PSRAM boards place the big `CrackedManifest` in the PSRAM heap, **and every no-PSRAM board keeps it in
static `.bss`**. That "otherwise static" clause silently assumed the only no-PSRAM board was the S3
Cardputer, which links the two manifests at 46.4% RAM. The CYD breaks the assumption.

**Measured.** Building the `cyd_3248s035c` env with the static `.bss` path (ADR-0080's no-PSRAM branch)
fails at link:

```
.dram0.bss will not fit in region dram0_0_seg
region dram0_0_seg overflowed by 27056 bytes
```

This is the same class of failure slice-0079 measured on the Stick (28816 bytes), for the same reason: a
classic ESP32's tighter usable static-DRAM budget cannot hold **two** ~28 KB `CrackedManifest` instances
in `.bss` (a `CrackedEntry` is 112 B, `entries_[256]` is 28672 B, two instances ≈ 56 KB). The CYD's number
is slightly smaller than the Stick's because the CYD ships headless (`hasDisplay=false`), so it compiles no
display `.bss`. Unlike the Stick, the CYD **has no PSRAM** to move the buffers to, so ADR-0080's PSRAM-heap
fix does not apply.

The two instances are `g_manifest` in `hunt_loop.cpp` (the hunt's cracked mirror) and the Maintenance
manifest in `main.cpp` `runMaintenance`. The decisive fact: **they are never live at the same time.** Hunt
and Maintenance are mutually-exclusive boot phases (§4 #21; Maintenance reboots into Station to leave). Both
occupy `.bss` for the whole process anyway, because static storage is reserved at link time regardless of
which phase runs. So ~56 KB of classic-ESP32 static DRAM is reserved for two objects of which at most one is
ever used.

**The measurable question** the fix hinges on: is a 28 KB *contiguous* internal-DRAM block available at the
point the manifest is allocated? Measured on device at the Provisioning stage (SoftAP up, a heavier heap
state than the Station-mode hunt path where the alloc actually happens):

```
[STATE] phase=provisioning heap_free=245888 heap_max=110580 disp=0x0
```

`heap_max` (largest free block) is **110580 B**, nearly 4x the 28 KB needed. `disp=0x0` confirms the
headless NullDisplay path, and no `[FATAL] display init failed` line appears (a screenless board is the
designed state, not a fault, ADR-0045).

## Decision

1. **On a no-PSRAM classic ESP32, the large `CrackedManifest` is placed in the internal DRAM heap at
   runtime, one object at a time, selected by `-DSAPPER_MANIFEST_HEAP_INTERNAL`.** This is the third case of
   ADR-0080's placement rule, not a replacement of it:
   - `BOARD_HAS_PSRAM` -> PSRAM heap (`MALLOC_CAP_SPIRAM`), unchanged (ADR-0080).
   - `SAPPER_MANIFEST_HEAP_INTERNAL` -> internal DRAM heap (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`), new.
   - neither -> static `.bss`, unchanged (the S3 Cardputer, byte-for-byte as it shipped).

   The macro names the **constraint**, not the chip: "this board's static budget cannot hold the dual
   manifests and it has no PSRAM." It is set in the env alongside `board`/`flash_size`, the same layer as
   `BOARD_HAS_PSRAM` (consistent with ADR-0002: a build-time capability selector).

2. **Why internal heap fits where static `.bss` did not — and it is not "the heap has more room."** The
   heap lives in the same internal DRAM pool as `.bss`; it is not a larger pool. The win is that a runtime
   allocation materialises **only the one manifest the current boot phase uses**, while static storage
   reserves **both** at link time. Removing both static objects returns ~56 KB to the pool; the one live
   manifest then takes 28 KB back at runtime. The link overflow (27056 B) is gone with margin, and the
   measured 110580 B largest free block confirms the 28 KB runtime block is available. The mutual exclusion
   of the hunt and Maintenance phases (§4 #21) is what makes "one at a time" true.

3. **Placement stays at whole-object granularity; the pure policy and its tests are untouched** (ADR-0080
   decision 2, unchanged). The device caller owns a pointer and placement-new's the object onto the chosen
   heap; where the bytes live is the caller's concern, not `CrackedManifest`'s. The native suite is
   unchanged by construction.

4. **A null internal-heap allocation fails loud, never a silent fallback** (ADR-0080 decision 4, extended).
   `heap_caps_malloc(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` returning null means the 28 KB block the link
   assumes is free is not, a real fault on a device that must hold the recovered-password mirror to do its
   job. The hunt caller returns false (the sibling of the store-mount failures), the Maintenance caller
   logs `[FATAL]` and `ESP.restart()`s. There is nothing to fall back to: a static `.bss` object is exactly
   what overflowed the link, so a fallback would reintroduce the fault it is escaping (§3 fail-loud; the
   no-degradation sibling of ADR-0045's cosmetic panel path).

5. **Scope: the big `CrackedManifest` only** (ADR-0080 decision 3, unchanged). The ~3 KB `SubmittedManifest`
   stays static on every board. The placement glue is a three-way branch inlined at its two sites
   (`hunt_loop.cpp` + `main.cpp`). It is **not** extracted into a shared helper: the static case needs a
   distinct `.bss` object per site, and a function-local `static` inside a shared helper templated on
   `CrackedManifest` would collapse the two sites onto one shared object (wrong). This is the same reason
   ADR-0080 inlined it; the rule-of-three helper remains deferred to a genuine *third placed buffer type*,
   not a third placement *case* of the same buffer (LEDGER).

6. **Static `.ext_ram.bss` is moot here and deferred for the heap cases** (ADR-0080 decision 5, unchanged).
   A no-PSRAM board has no external RAM segment, so the attribute form never applied to this case; the
   internal-heap route is the end state for a no-PSRAM classic ESP32, not an interim one.

## Consequences

- The CYD links: `.dram0.bss` fits (RAM 28.8%, 94248 B of 327680), the 27056-byte overflow gone. The link
  is itself the machine check, as it is for the Stick (§4 #36): an over-budget `.bss` is a hard build
  failure, so "overflow-gone" is asserted on every `cyd_3248s035c` build.
- `hunt_loop.cpp` and `main.cpp` gain a third placement branch (`#elif defined(SAPPER_MANIFEST_HEAP_INTERNAL)`)
  and a matching `[FATAL]` guard; every manifest *use* still goes through the pointer uniformly on all
  boards. No pure module changes, so the native suite is unchanged by construction.
- A new §4 standing invariant (#37) records the three-way rule so a future no-PSRAM classic board follows it
  and a future reader does not "simplify" the internal-heap branch back into the static `.bss` path that
  overflows, nor the fail-loud guard into a fallback.
- The display HAL (`lgfx_display.h/.cpp`) is now guarded by `SAPPER_BOARD_HAS_DISPLAY`, so a headless board
  (the CYD this slice) compiles no LovyanGFX class at all — making the long-standing "a screenless board
  compiles no LGFX_* class" contract (ADR-0002 #4) true at the file level, not only at main.cpp's include
  guard. A board that declares a display but forgets its panel config still hits the hard `#error`.
- The full end-to-end proof that the manifest is allocated from the internal heap *and* read correctly by a
  live hunt and the Maintenance dashboard needs a provisioned device (real WiFi + a wpa-sec account) and is
  the operator's to run, like ADR-0080's PSRAM boot proof. The headless Provisioning boot (above) plus the
  measured 110580 B largest free block already establish the block is available; the provisioned run
  confirms the object is used. Tracked in the LEDGER alongside the Stick's deferred PSRAM boot proof.
