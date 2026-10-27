---
id: '0080'
title: "Per-board large-buffer memory placement: runtime PSRAM heap on PSRAM boards, static DRAM otherwise"
type: architecture
status: accepted
date: 2026-10-27
supersedes: []
superseded_by: []
---

## Context

slice-0079 brings up the M5StickC Plus2 (ESP32-PICO-V3-02), the first PSRAM board in the tree. Its
device link **failed**: `.dram0.bss` overflowed `dram0_0_seg` by **28816 bytes**. The reference
Cardputer (ESP32-S3) links the same firmware at 46.4% RAM, so this is not a firmware-wide bloat: it is
the classic ESP32's tighter usable static-DRAM budget meeting two large `.bss` buffers that the S3 had
the slack to carry.

The two buffers, measured: both are `CrackedManifest` instances, each a fixed `CrackedEntry entries_[256]`
array (essid + psk + two timestamps per entry, ~112 B each, so ~28 KB per instance):

- `g_manifest` in `hunt_loop.cpp` (file-scope, the hunt's cracked mirror), and
- the Maintenance manifest in `main.cpp` `runMaintenance` (a function-local `static`, deliberately static
  because 28 KB overflows the 8 KB loopTask stack).

Both occupy `.bss` for the whole process regardless of which boot phase runs (hunt vs Maintenance are
mutually exclusive phases, §4 #21, but their storage is reserved at link time either way), so ~56 KB of
classic-ESP32 static DRAM is spent on cracked mirrors. The sibling `SubmittedManifest` (`g_submitted`,
~3 KB, since a `SubmittedEntry` carries no essid/psk) is one tenth the size and is not a material
contributor.

The board has 2 MB Quad PSRAM, and the ESP32-PICO-V3-02 is ECO-V3 silicon, so the historical ESP32 PSRAM
cache bug is fixed in hardware (no `CONFIG_SPIRAM_CACHE_WORKAROUND` needed). The obvious fix is to place
the big buffers in PSRAM on PSRAM boards. The question is *how*, and that has a measured answer, not an
assumed one.

**Mechanism probe (read the precompiled libs' baked sdkconfig, not guessed):** the Bruce patched Arduino
libs this repo depends on (`framework-arduinoespressif32-libs`, classic `esp32`) ship:

- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` **is not set**, so static `.ext_ram.bss` placement
  (`EXT_RAM_BSS_ATTR` on a buffer or object) **cannot link**. The clean "attribute on a POD array"
  approach is unavailable without rebuilding the patched IDF libs, which are a precompiled third-party
  blob (deferred: the shared patched-libs initiative, LEDGER, in both this repo and the Adversary).
- `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` **are set**,
  so PSRAM is initialised at boot and joined to the heap, and `heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`
  places a block in PSRAM deterministically at runtime. `-DBOARD_HAS_PSRAM` (already on the Stick env)
  makes Arduino's `initArduino()` run `psramInit()` before `setup()`.

So the only mechanism available with the libs we have is **runtime PSRAM heap allocation**.

## Decision

1. **A large persisted-manifest buffer is placed in PSRAM on a PSRAM board and in static DRAM on a
   no-PSRAM board, selected at compile time by `BOARD_HAS_PSRAM`** (consistent with ADR-0002: branch on
   what a board *has*, never what it *is*). PSRAM boards allocate with `heap_caps_malloc(sizeof(T),
   MALLOC_CAP_SPIRAM)` and placement-new; no-PSRAM boards keep today's static `.bss` object verbatim.

2. **Placement is at whole-object granularity, not by externalising the array.** The device caller owns a
   pointer to the manifest; on a PSRAM board it points at a PSRAM-heap object, on a no-PSRAM board at the
   same static object as today. The pure, host-tested `CrackedManifest` policy and **all** its unit tests
   are left untouched: where the bytes live is the caller's concern, not the policy's. This keeps the
   no-PSRAM path byte-for-byte as it shipped (zero regression risk on the reference Cardputer) and avoids
   a speculative constructor-signature change to a pure class whose storage is fixed by design.

3. **Scope: the big `CrackedManifest` only.** Both cracked instances (hunt in `hunt_loop.cpp`, Maintenance
   in `main.cpp`) are PSRAM-placed on PSRAM boards; that frees ~56 KB of classic-ESP32 static DRAM, well
   clear of the 28816-byte overflow. The ~3 KB `SubmittedManifest` stays static DRAM on every board: it is
   below the size that matters and heap-indirecting it would add a second footgun for a tenth of the
   payoff (KISS). The placement glue is inlined at its two sites (hunt + Maintenance); a **third**
   PSRAM-placed buffer triggers extracting a shared board-aware placement helper (rule of three, LEDGER),
   not the second.

4. **A null PSRAM allocation fails loud, never silently.** `heap_caps_malloc(MALLOC_CAP_SPIRAM)` returning
   null on a board built `BOARD_HAS_PSRAM` means PSRAM is absent or uninitialised, a hardware/build
   mismatch. The caller logs `[FATAL]` and stops using the object (hunt: returns false, the sibling of the
   store-mount failures; Maintenance: `ESP.restart()`) rather than falling back to a DRAM allocation: a
   28 KB DRAM block is exactly what overflowed at link, so a silent DRAM fallback would either fail at
   runtime or mask the misconfiguration (§3 fail-loud; the no-degradation sibling of ADR-0045's panel path,
   which *can* degrade to headless because a missing panel is cosmetic, where a missing manifest is not).

5. **Static `.ext_ram.bss` placement is the mechanism we would prefer and is deferred, not rejected.**
   When the shared patched-libs initiative (LEDGER, both repos) lets us own the IDF build and set
   `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`, `EXT_RAM_BSS_ATTR` becomes a one-line static attribute
   with no runtime allocation, no fail-loud path, and no pointer indirection. At that point this ADR is
   **superseded** by the attribute form, and that supersession note will record that runtime heap was only
   ever the interim mechanism the precompiled libs forced. Recording it now is why the later switch has a
   reason instead of looking like churn.

## Consequences

- The M5StickC Plus2 links: the `.dram0.bss` overflow is gone (the two 28 KB cracked objects leave DRAM
  for PSRAM). The link is itself the machine check: an over-budget `.bss` is a hard build failure.
- `hunt_loop.cpp` and `main.cpp` gain a board-conditional pointer + PSRAM placement-new + `[FATAL]` guard;
  every manifest *use* goes through the pointer uniformly on both boards. No pure module changes, so the
  native suite is unchanged by construction.
- A new §4 standing invariant (#36) records the placement rule so a future PSRAM board follows it and a
  future reader does not "simplify" the fail-loud guard into a DRAM fallback.
- On-device PSRAM boot proof is required and is the operator's to run (it cannot be asserted headlessly):
  confirm `psramFound()`, that the manifest objects land in PSRAM (heap-caps accounting), and that both
  the hunt and the Maintenance dashboard read the cracked mirror correctly from PSRAM. This run doubles as
  the SUB1->SUB2 store-migration proof the LEDGER tracks for the first PSRAM board.
