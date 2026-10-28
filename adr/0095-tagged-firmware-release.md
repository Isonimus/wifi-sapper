---
id: '0095'
title: "Tagged firmware release with per-chip-family factory images"
type: architecture
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Context

The Sapper ships to operators through m5burner and hand flashing, but the repo publishes no release
artifact: an operator must build from source. A tagged GitHub Release carrying ready-to-flash images is
the distribution path, and CI already builds every board on each push (the per-push matrix), so a tag can
reuse that to package and publish.

The parent project does exactly this (`adversary:ADR` / its slice-0014 release workflow), but **cardputer
only**, a single ESP32-S3 target. The Sapper cannot copy it verbatim for one load-bearing reason: it spans
**two chip families**. The Cardputer and T-embed are ESP32-S3; the M5StickC Plus2 and the CYD are
**classic ESP32**. The two families place the 2nd-stage bootloader at **different flash offsets** (S3 at
`0x0`, classic ESP32 at `0x1000`), so a factory (merged) image built with one family's offsets is wrong
for the other. A merge that hardcodes `0x0` would produce a classic-ESP32 image that never boots.

Two facts from the parent carry over:

- The **dio-not-qio brick trap** (`adversary:slice-0014`): the ROM reads the 2nd-stage bootloader using
  the flash mode in the bootloader header *before* quad mode is enabled, so a `qio`-headed factory image
  makes the ROM misread the bootloader into a reset loop, a hard brick that `esptool image_info` does not
  catch. PlatformIO's own flasher converts `qio`->`dio` when writing; a hand-rolled `merge-bin` bypasses
  that, so the merged image must be built `dio`-headed (qio is enabled at runtime, not in the header).
- The release reuses CI's warmed PlatformIO cache (same cache key as the per-push build leg).

Two things the Sapper lacks: there is **no log-level knob** (no `SAPPER_LOG_LEVEL` equivalent), so there
is no reduced-logging release env like the parent's `cardputer_release`; and there is **no web flasher**
(no Pages workflow).

## Decision

1. **A `release.yml` on `v*` tags publishes a GitHub Release.** It builds each released board's normal env
   and packages, per board: the **OTA app image** (`firmware.bin` unmodified), a **full factory (merged)
   image** for a fresh esptool / web-flasher install at the chip's base offset, and a **SHA256SUMS**;
   `gh release create --generate-notes` writes them to Releases with the built-in `GITHUB_TOKEN`.

2. **The first release covers all four supported boards, across both chip families:** Cardputer and
   T-embed (ESP32-S3, bootloader `0x0`), M5StickC Plus2 and CYD (classic ESP32, bootloader `0x1000`). All
   four are verified on hardware (boot + associate + hunt + HUD eyes-on) before the release (slice-0096), so
   a factory image is published only for a board confirmed working; the per-chip packaging is exercised on
   both families by construction.

3. **Factory-image offsets branch per chip family, carried as matrix data.** The merged image places the
   2nd-stage bootloader at **`0x0` on S3** and **`0x1000` on classic ESP32**; the other offsets are the
   standard layout both families share (partition table `0x8000`, otadata/`boot_app0` `0xe000`, app
   `0x10000`). Each matrix row carries its `--chip`, bootloader offset, and `--flash-size` (Cardputer 8MB,
   Stick 8MB, T-embed 16MB, CYD 4MB). `boot_app0.bin` is located under the installed framework package,
   never hardcoded.

4. **The dio brick-guard is mandatory on every factory image.** After merging, the workflow reads the
   bootloader header **at the chip family's bootloader offset** (not file offset 0: a classic-ESP32 image
   is `0xff`-padded from `0x0` to `0x1000`, a cross-chip trap slice-0096's probe measured) and **refuses to
   publish** unless the image magic there is `0xE9` (proof the bootloader landed at the right offset) and
   the flash-mode byte (byte 2 of the header) is `0x02` (dio). The images are built dio for flashing even
   though qio is enabled at runtime. A non-dio image is a hard brick `esptool image_info` cannot detect, so
   the guard is the last line before an operator flashes a bricking image.

5. **Ship the normal env build; no release env this release.** With no log-level knob, there is nothing to
   strip; a reduced-logging release env is deferred (LEDGER). The OTA app image is the normal
   `firmware.bin`.

6. **No web flasher this release.** The artifacts (version-named app + factory + SHA256SUMS) serve esptool,
   the browser ESP web flasher by file, and m5burner submission. A Pages web flasher (and the
   release->Pages cascade quirk the parent documented) is deferred (LEDGER).

This **extends** ADR-0002 (per-board build config drives per-board packaging) and ADR-0004 (CI as the
authority). It adapts `adversary`'s release workflow to a two-chip-family tree and **supersedes none**.

## Consequences

- **A new §4 standing invariant (#42):** a published factory image is built `dio`-headed and uses its chip
  family's bootloader offset (`0x0` S3 / `0x1000` classic ESP32); the release workflow refuses a non-dio
  image. This binds every matrix row and any future board leg: whoever adds one must use the right offset
  for the family or the guard/booting fails. The linter does not read `.github/` (§4 #26), so it is
  review-only plus the workflow's own in-step check.
- `release.yml` is added; it triggers only on `v*` tags, so it never runs on an ordinary push and does not
  change the per-push CI gate. `README.md` gains a short "install a release" note.
- Verification is a local packaging probe (build + `merge-bin` + the dio-header check for one S3 and one
  classic board, the measured proof that both families' offsets are right before the workflow is trusted),
  plus the first real `v*` tag; a release workflow cannot dry-run without a tag, so there is no wired
  verify script (it is not a runtime behaviour).
- Deferrals recorded in `LEDGER.md`: a reduced-logging release env (needs a log-level knob first) and a
  Pages web flasher. (The T-embed/CYD on-air display proofs deferred by slices 0085/0087/0088 are closed by
  slice-0096's four-board verification.)
