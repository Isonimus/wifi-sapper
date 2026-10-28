---
id: '0096'
title: "Release workflow: tagged firmware images for Cardputer and the Stick"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Publish ready-to-flash firmware on a `v*` tag, so an operator can install the Sapper without building from
source (m5burner, esptool, or the browser ESP web flasher by file). Implements ADR-0095: a `release.yml`
that builds each released board, packages an OTA app image + a full factory image (offsets per chip family)
+ SHA256SUMS, enforces the dio brick-guard, and cuts a GitHub Release. This first release covers all four
supported boards across both chip families: Cardputer and T-embed (ESP32-S3), M5StickC Plus2 and CYD
(classic ESP32), each verified on hardware (boot + associate + hunt + HUD eyes-on) before the release.

Picks up the LEDGER feature item for a release stage (the CI comparison against the parent). It does not
close the slice-8 hardware-broadening umbrella.

## Definition of Done

- **Given** a `v*` tag pushed to the repo
- **When** `release.yml` runs
- **Then** a GitHub Release is created for that tag carrying, for each of `cardputer`, `m5stickc_plus2`,
  `t_embed`, and `cyd_3248s035c`: a version-named app image, a version-named factory image, and a shared
  `SHA256SUMS.txt`.

- **Given** the factory image for each board
- **When** the workflow inspects its 2nd-stage bootloader header before publishing
- **Then** the flash-mode byte is `0x02` (dio); a non-dio image **fails the job** and nothing is published
  (the brick-guard), and the S3 images (Cardputer, T-embed) use bootloader offset `0x0` while the
  classic-ESP32 images (Stick, CYD) use `0x1000`.

- **Given** an ordinary push or PR (no tag)
- **When** CI runs
- **Then** `release.yml` does not run (it is `v*`-tag-only) and the existing per-push gate is unchanged.

## Design

**Existing code considered.** Modelled on the parent's cardputer-only release workflow
(`adversary:slice-0014`): the `gh release create --generate-notes` shape, the warmed-cache key reuse, the
`boot_app0.bin` lookup under the framework package, and the dio-header guard are taken from it. What is
**new** for the Sapper: a board **matrix** carrying per-chip data (`--chip`, bootloader offset,
`--flash-size`), because the two families differ at `0x0` vs `0x1000` (ADR-0095). What is deliberately
**not** copied: the parent's `cardputer_release` reduced-logging env (the Sapper has no log-level knob, so
the normal env ships) and its Pages web-flasher redeploy step (no web flasher here) — both deferred (LEDGER).

**Matrix.** Four rows:

| env | chip | bootloader offset | flash size |
|---|---|---|---|
| `cardputer` | esp32s3 | `0x0` | 8MB |
| `m5stickc_plus2` | esp32 | `0x1000` | 8MB |
| `t_embed` | esp32s3 | `0x0` | 16MB |
| `cyd_3248s035c` | esp32 | `0x1000` | 4MB |

Shared offsets: partition table `0x8000`, otadata/`boot_app0` `0xe000`, app `0x10000`.

**Packaging per row** (in the job): `pio run -e <env>`; copy `firmware.bin` to
`sapper-<env>-<version>-app.bin`; `esptool --chip <chip> merge-bin -o sapper-<env>-<version>-factory.bin
--flash-mode dio --flash-freq 80m --flash-size <size> <bootloader-offset> bootloader.bin 0x8000
partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin`; then, **at the chip's bootloader offset** (not
file offset 0: a classic-ESP32 image is `0xff`-padded to `0x1000`, measured), assert the magic is `e9` and
byte+2 is `02` (dio) or fail; `sha256sum` the images. `gh release create <version> --generate-notes` with all rows'
artifacts (the matrix fans out build+package; a final job, or `--generate-notes` on a collected set,
assembles the release — see the workflow for the exact fan-in).

**Safety.** esptool is pinned (deterministic merge-bin). The dio guard is the DoD's scenario 2 and the
reason a bricking image can never be published.

## Verification

A release workflow cannot be dry-run without a real tag, and it is a build/packaging job, not a runtime
behaviour, so there is no wired `scripts/*-verify.mjs` (R11 exempt, like a probe).

- **Local packaging probe (measured, pre-commit):** build all four envs, run the exact `merge-bin` the
  workflow uses for each (S3 `0x0`, classic `0x1000`), and assert each factory image's bootloader-header
  byte is `02` and `esptool image_info` parses it. This proves both chip families' offsets and the dio guard
  before the workflow is trusted; the measured result is recorded in `## As built`.
- **On-air board verification (operator-run):** each board flashed, provisioned, and confirmed to associate
  and hunt (serial `[STATE]`/`[HUNT]`) with its HUD rendering correctly (T-embed roomy tier; CYD
  direct-panel), closing the deferred display proofs so a factory image is published only for a working board.
- **YAML validity (machine-checked):** the workflow parses as valid YAML and its matrix lists exactly the
  two boards with their per-chip data.
- **First real tag (operator-run):** the first `v*` tag produces the Release; the in-workflow dio guard is
  the per-run check thereafter. DoD scenario 1 is confirmed on that first tag.
- **No per-push impact:** `release.yml` is `on: push: tags: v*` only, so DoD scenario 3 holds by construction.

## As built

Shipped as designed: `release.yml` with a four-board matrix, per-chip-family bootloader offsets, and the
dio brick-guard read at each chip's bootloader offset. DoD scenario 1 (the Release with app + factory +
SHA256SUMS per board) and scenario 3 (no per-push impact: `on: push: tags: v*` only) hold by construction;
scenario 2 (the dio guard) is proven by the packaging probe below and runs per-tag in the workflow.

**Cross-chip bug caught by the probe (measure-don't-estimate).** The first probe read the flash-mode byte at
**file offset 2**, which passed the S3 boards but is wrong for classic ESP32: a classic-ESP32 merged image is
`0xff`-padded from `0x0` to `0x1000` (the bootloader lives at `0x1000`), so offset 2 read `0xff`, a NOT-DIO
false positive that would have let a genuinely qio-headed classic image through. Fixed by reading both the
magic and the mode byte **at the chip family's bootloader offset** (`boot=$(( boot_offset ))`), and recorded
the offset trap in ADR-0095 decision 4 and §4 #42. The workflow guard and the probe now agree.

**Local packaging probe (measured, pre-commit).** Built all four envs, ran the exact `esptool v5.2.0
merge-bin` the workflow uses (`--flash-mode dio --flash-freq keep`, per-family offset), and read the header at
each board's bootloader offset. All four: magic `0xe9` (bootloader landed at the right offset for the family)
and flash-mode `0x02` (dio).

| env | chip | bootloader offset | flash size | magic @ offset | mode @ offset+2 |
|---|---|---|---|---|---|
| `cardputer` | esp32s3 | `0x0` | 8MB | `0xe9` | `0x02` (dio) |
| `m5stickc_plus2` | esp32 | `0x1000` | 8MB | `0xe9` | `0x02` (dio) |
| `t_embed` | esp32s3 | `0x0` | 16MB | `0xe9` | `0x02` (dio) |
| `cyd_3248s035c` | esp32 | `0x1000` | 4MB | `0xe9` | `0x02` (dio) |

Both chip families' offsets and the dio guard are thus proven before the workflow is trusted.

**On-air board verification (2026-10-28).** Each board's hunt firmware confirmed on hardware; a factory image
is published only for a board confirmed working:

- **M5StickC Plus2** (classic ESP32, compact 240x135 tier): verified earlier this session: boot, associate,
  hunt, HUD eyes-on, plus the battery indicator (`verify:battery`) and inactivity dimming (`verify:dim`)
  proven on this board (operator-confirmed on panel).
- **T-embed** (ESP32-S3, roomy 320x170 tier): flashed this session; booted (`disp=320x170`), exhausted its
  stale creds on `NO_AP_FOUND` and correctly fell back to `phase=provisioning` (SoftAP `Sapper-7E98`), was
  re-provisioned, then associated and hunted with deauth TX working (`txOk` climbing, `txFail=0`, capture
  windows cycling). Operator confirmed the full-panel roomy HUD renders perfectly. Closed the slice-0088 HUD
  proof. Its battery divider stays uncalibrated (`batteryDividerPermille=0`), so the icon is correctly hidden
  (§4 #40); measuring it is deferred (LEDGER).
- **CYD** (classic ESP32, direct-panel 480x320): flashed this session; booted through
  `station_connect -> time_sync -> ready` (`disp=480x320`, `heap_max=110580` matching the ADR-0082/0084
  internal-heap budget), `[SCREEN] enabled`, no `[FATAL]`, and hunted with deauth TX working. Operator
  confirmed the direct-panel HUD renders flicker-free (no whole-panel heartbeat flash). Closed the slice-0087
  HUD proof.
- **Cardputer** (ESP32-S3, reference target): recorded as reference-verified rather than re-flashed this
  session (operator's call): it is the continuously-verified reference every prior on-air verify script ran
  against, and its factory image passed the probe above.

**YAML validity (machine-checked).** `release.yml` parses as valid YAML; the build matrix lists exactly the
four boards with their per-chip `chip`/`boot_offset`/`flash_size`, and the trigger is `v*` tags only.

**Adversarial review (Sonnet, blind to intent).** A blind reviewer read `release.yml` against `CLAUDE.md`/
`platformio.ini` and confirmed the offsets, flash sizes, the dio guard (magic + mode at the per-family offset,
fail-loud under `set -euo pipefail`), the fan-out->fan-in abort-on-any-board-failure, and the `v*`-only
trigger. It found two issues, both fixed pre-merge: (1) `gh release create` is not transactional, so a
mid-upload failure could leave a **partial public** release: fixed by creating as `--draft` and flipping to
public in one metadata edit only after every asset uploaded; (2) `find boot_app0.bin | head -1` could pick the
wrong framework version after a cache restore-key fallback: fixed by asserting exactly one match and failing
loud otherwise.

**Deferred to the first real tag (operator-run).** A release workflow cannot run without a real tag, so DoD
scenario 1 is confirmed end-to-end on the first `v*` push; the in-workflow dio guard is the per-run check
thereafter.
