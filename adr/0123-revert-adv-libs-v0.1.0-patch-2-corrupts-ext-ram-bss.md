---
id: '0123'
title: "Revert the esp32-arduino-adv-libs v0.1.0 adoption: patch #2 corrupts the esp32 core's .ext_ram.bss"
type: architecture
status: superseded
date: 2026-11-15
supersedes: ['0121']
superseded_by: ['0124']
---

## Context

ADR-0121 adopted the owned `esp32-arduino-adv-libs` v0.1.0 build on all five envs and retired the local
deauth-weaken hook; slice-0122 shipped it, and it was merged and pushed to `main`. ADR-0121 decision 4 made
the per-env *link* the machine check and decision 5 deferred the PSRAM boot behaviour to an on-device proof.
The link passed and the native suite passed, so the merge looked clean. The on-device run is what caught the
defect the link could not.

**Measured on hardware (2026-11-15), every esp32 board crash-loops on the plain v0.1.0 firmware:**

- `m5stickc_plus2` (esp32 + 2 MB PSRAM): `Guru Meditation Error ... LoadProhibited`, `EXCVADDR=0xaaaaa9dc`
  (the `0xaa` uninitialised-memory fill), reaching `[STATE] phase=provisioning` then panicking, ~7 reboots in
  12 s. The faulting register dump dereferences objects at `0x3f8019xx` (the esp32 PSRAM `.ext_ram.bss`
  range) holding `0xaaaa…` garbage pointers.
- `cyd_3248s035c` (esp32, **no** PSRAM): identical `LoadProhibited` crash loop (`EXCVADDR=0xa4a52019`,
  `heap_max=110580` confirming no PSRAM), reaching `[STATE] phase=station_connect` then panicking.
- `atom_echo` (esp32, no PSRAM): same board class as the CYD and the same config, so the same failure (not
  separately crash-tested; confirmed working once reverted).
- `t_embed` and `cardputer` (esp32s3) are **unaffected** and ran clean, because patch #2 is esp32-only.

**Root cause (confirmed, not assumed).** v0.1.0 patch #2 sets `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`
on the esp32 build (esp32-adv-libs:ADR-0021). That flag makes the `EXT_RAM_BSS_ATTR` macro active, so the
**Arduino core's own** `EXT_RAM_BSS_ATTR` globals (~16 KB measured) are placed in the external-RAM BSS
segment at PSRAM addresses (`0x3f800000+`). But the same v0.1.0 esp32 build ships PSRAM boot-init **off**
(`# CONFIG_SPIRAM_BOOT_HW_INIT is not set`, the esp32s3 PSRAM-less-reference-board safety of
esp32-adv-libs:ADR-0015, applied to esp32 too), so PSRAM comes up late via Arduino's `psramInit()` and the
`.ext_ram.bss` segment is **never zeroed**. The core then reads an uninitialised pointer out of
`.ext_ram.bss` and dereferences it. `ALLOW_BSS=y` with PSRAM not initialised at boot is an **invalid
sdkconfig combination**; the two defconfig appends are individually reasonable but mutually incompatible on
esp32. The Bruce libs shipped `ALLOW_BSS` **unset**, so `EXT_RAM_BSS_ATTR` was a no-op and the core's globals
sat in normal, zeroed internal DRAM `.bss`: that is why every esp32 board worked before the adoption.

The probe that found this (slice-0122 Change-2 investigation) also measured that a **POD** `EXT_RAM_BSS_ATTR`
buffer is placed at `0x3f800000` and is writable but reads back **non-zero** (`bytesum=141016`), directly
confirming `.ext_ram.bss` is not zeroed under this config.

This corrects two earlier claims:

- **ADR-0121 decision 3** framed patch #2 as merely "esp32-only and useful for a consumer `EXT_RAM_BSS`
  buffer." It is worse than useless on esp32: it is actively corrupting, because the flag's effect is not
  limited to opt-in consumer buffers; it changes where the *core's own* globals land, into memory the build
  never zeroes.
- **ADR-0080 decision 5** assumed that owning the libs and setting `ALLOW_BSS` would make the static
  `EXT_RAM_BSS_ATTR` form available. The Change-2 probe already showed the clean static form is infeasible
  (the manifest is polymorphic with a store-bearing constructor that would run against unmapped PSRAM before
  the late `psramInit()`); this ADR adds that the flag which was supposed to enable it is itself unsafe. The
  runtime `MALLOC_CAP_SPIRAM` route (ADR-0080) is the end-state mechanism on esp32 PSRAM boards, not an
  interim one.

## Decision

1. **Revert the v0.1.0 adoption on all five envs.** Restore the Bruce `framework-arduinoespressif32-libs`
   blob URLs and the local deauth-weaken hook (`scripts/weaken_deauth_pre.py` + `scripts/weaken_deauth_symbol.sh`
   and its `extra_scripts` wiring), exactly as they were before slice-0122, so `main` is working on every
   board again. Remove §4 invariant #52 (ADR-0121's "weak symbol is a pinned-artifact property" claim), which
   is false once the hook is back. All five revert, not just the esp32 three, so the tree is one coherent
   pre-adoption state rather than a mixed one.

2. **ADR-0121 and slice-0122 are kept as immutable records and marked superseded by this ADR** (not deleted),
   per the stele method: the adoption happened, the on-device finding is the valuable part, and a literal
   `git revert` that erased them would destroy exactly the record this workflow exists to keep.

3. **Re-adoption is blocked on an upstream fix in esp32-arduino-adv-libs.** The fix is to make patch #2 safe
   or withdraw it. Since the Change-2 probe already showed the static `EXT_RAM_BSS_ATTR` form is infeasible
   for this firmware (polymorphic manifest + late `psramInit()`), patch #2 delivers **no usable benefit** to
   this consumer and this one real harm, so the recommended upstream action is to **withdraw patch #2**
   (`esp32-adv-libs:ADR-0021`) rather than try to also enable `.ext_ram.bss` zeroing on esp32. Tracked in the
   LEDGER. Patch #1 (the weak symbol) is unaffected and still desirable; a future release carrying patch #1
   without patch #2 is re-adoptable (and would retire the weaken hook as slice-0122 intended).

4. **On-device boot is now a required gate for any future libs adoption, not a deferred one.** ADR-0121
   deferred the esp32 boot proof to "the operator's to run"; this incident shows a libs swap that passes the
   link and the native suite can still brick every board of one chip family. A future re-adoption flashes and
   boots at least one esp32 board and one esp32s3 board **before** merge, not after.

## Consequences

- `main` returns to the known-good Bruce libs on all boards; the esp32 crash loop is gone and the weaken hook
  is back in force (the strong-symbol link is restored).
- ADR-0121 carries `superseded_by: ['0123']`; slice-0122 gains a rollback amendment. The on-device evidence
  and the root cause live on in both this ADR and that amendment.
- The LEDGER's shared-libs feature line returns to "not yet adopted" with the blocker named (patch #2), and
  the Change-2 item is closed into this ADR's recommendation to withdraw patch #2 upstream.
- The sibling Adversary repo, which had not yet adopted v0.1.0, must not adopt it until the upstream fix
  lands; its LEDGER mirror carries the same blocker.
- A measured lesson is recorded for the method: a link-green, tests-green dependency swap is not proven until
  it boots on real hardware of each affected chip family.
