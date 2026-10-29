---
id: '0102'
title: "Browser web flasher page (ESP Web Tools on GitHub Pages)"
type: slice
status: accepted
date: 2026-10-29
supersedes: []
superseded_by: []
---

## Goal

Collapse first-flash onboarding from "install Python + PlatformIO, resolve the toolchain,
`pio run -t upload`" to "open a page, pick your board, click Install". A static page hosting
[ESP Web Tools](https://esphome.github.io/esp-web-tools/) flashes the firmware straight from the
browser over Web Serial, with no local toolchain.

This is the consumer half of the release work. ADR-0095 / slice-0096 already build and publish the
merged, `dio`-headed, per-chip-family-offset **factory images** for all four boards on a `v*`-tagged
release, validated by the in-workflow bootloader-magic + flash-mode guard. The factory image is
exactly what a web flasher writes to `0x0`. So this slice adds no build or packaging logic: only the
page, the per-board manifests, the Pages deploy, and the release wiring to keep them current.

This picks up the LEDGER web-flasher item (ADR-0051 decision 7 deliberately deferred a Pages site
for an alpha; the release half is now done, this is the missing flasher page).

## Definition of Done

- **Given** a supported browser (desktop Chrome/Edge/Opera) on the hosted HTTPS page, a board
  selected in the picker, and that board connected over USB
- **When** the operator clicks *Connect & Flash*
- **Then** ESP Web Tools reads that board's manifest, fetches its factory image same-origin, and
  flashes it to `0x0`, installing the firmware with no local toolchain

- **Given** the operator selects a board whose chip family does not match the connected device (e.g.
  the classic-ESP32 CYD manifest against a connected ESP32-S3 Cardputer)
- **When** they attempt to flash
- **Then** ESP Web Tools refuses on the `chipFamily` mismatch rather than writing a wrong-chip image
  (the picker disambiguates same-family boards; `chipFamily` is the cross-family safety net)

- **Given** a firmware release is cut (a new `v*` tag) and Pages redeploys
- **When** the page is loaded afterwards, unchanged
- **Then** it serves the **newest** factory image for every board automatically (the deploy pulls the
  stable-named latest assets), with no page or manifest edit per release

- **Given** an unsupported browser (Firefox/Safari/mobile) or a non-HTTPS origin
- **When** the page loads
- **Then** the install control is replaced by an explanatory slot routing the operator to the manual
  `esptool`/PlatformIO fallback, with no dead button

## Design

### Ported wholesale from the parent (adversary:slice-0016)

The parent shipped and hardened this exact mechanism; we reuse its decisions rather than rediscover
them:

- Two-plus files under `web/`: `index.html` (self-contained, inline CSS, no build step) and the
  manifests, deployed to GitHub Pages by `.github/workflows/pages.yml` using **only first-party
  `actions/*`** (`configure-pages` -> `upload-pages-artifact` -> `deploy-pages`), adding no
  third-party action to the supply chain (ADR-0051 CI posture). One-time manual step: Settings ->
  Pages -> Source = GitHub Actions.
- **Same-origin firmware (the parent's hard-won correction).** GitHub release assets send no
  `access-control-allow-origin` header, so a browser cannot `fetch()` them cross-origin from the
  Pages origin ("Failed to fetch" after the chip handshake). The manifests therefore use **relative**
  paths, and `pages.yml` pulls the latest release's factory images into `web/` at deploy time
  (`gh release download`, not committed; `*.bin` git-ignored), failing loud if an asset is missing.
- **The dispatch cascade.** A release cut by `release.yml` uses the built-in `GITHUB_TOKEN`, whose
  emitted events GitHub does not cascade into other workflows (recursion guard), so the
  `release: published` trigger never fires for CI releases. `release.yml` must dispatch `pages.yml`
  explicitly as its final step (`gh workflow run pages.yml --ref main`, needing `actions: write`).
  `release: [published]` is kept only as a fallback for a human-published release.
- esp-web-tools is loaded as a **page** runtime dependency (the flash engine, unavoidably external),
  pinned to an exact version confirmed latest at build time.

### The one delta the parent did not face: four boards, two chip families

The parent flashes one board. We ship four (ADR-0095): Cardputer + T-embed are both **ESP32-S3**,
M5StickC Plus2 + CYD are both **classic ESP32**. ESP Web Tools auto-selects a `builds[]` entry **by
chip family only**, so a single auto-family manifest cannot tell a Cardputer from a T-embed and would
flash the wrong sibling's image (wrong pins, wrong display driver).

Decision: the page carries a **board picker** (a `<select>` defaulting to the reference Cardputer)
that points the single `<esp-web-install-button>` at that board's **own single-build manifest**.
Four tiny manifests, one per board, each declaring its real `chipFamily`:

- `manifest-cardputer.json` -> `ESP32-S3`, `./sapper-cardputer-factory.bin`
- `manifest-t_embed.json` -> `ESP32-S3`, `./sapper-t_embed-factory.bin`
- `manifest-m5stickc_plus2.json` -> `ESP32`, `./sapper-m5stickc_plus2-factory.bin`
- `manifest-cyd_3248s035c.json` -> `ESP32`, `./sapper-cyd_3248s035c-factory.bin`

Each part is at `offset: 0` (ADR-0095's merged factory image is `0x0`-based, with the bootloader
already at its chip-family offset inside the image). `chipFamily` then does double duty: within a
family the picker chooses the board, and across families ESP Web Tools refuses a mismatched chip
(DoD scenario 2). `new_install_prompt_erase: true` offers a full erase on install; unlike the
parent (captures on a separate SD), the Sapper's captures, cracked manifest, and provisioning live
on **internal LittleFS**, so a full erase wipes them. The page copy states this plainly.

### Stable-named release assets (the second release.yml delta)

`/latest/download/` and a same-origin pull both need a **fixed** asset name, but `release.yml`
publishes only version-stamped `sapper-<env>-<version>-factory.bin`. It gains, per board, an
unversioned copy `sapper-<env>-factory.bin` (a `cp` + one extra publish arg), byte-identical and
covered by the existing checksum step. These are the four names `pages.yml` downloads and the
manifests reference.

### Visual identity (banner-matched, distinct from the parent's red-team page)

The page reads as this product, not the parent: the palette is taken from
`docs/images/wifi-sapper-banner.webp` (a cyan + magenta duotone on near-black, not the Adversary's
`#ff2020` red). Tokens: ground `#06080f`, text `#cfeeff`, hairline `#14303a`, cyan accent `#28e6ff`
(primary CTA, waveform), magenta/violet accent `#c93cff`..`#ff3cf0` (secondary glow, glitch). Light
and dark are one dark theme (the product is a dark instrument panel); a HUD register (corner
brackets, hairlines, a scanline wash) echoes the banner.

Typography is **Chakra Petch** (wordmark, the squared notched-techno face closest to the banner
title) + **Rajdhani** (body/subtitle, condensed techno), **vendored** as woff2 under `web/fonts/`
rather than hotlinked, keeping the page self-contained (the parent's property) while matching the
banner, since the user asked for the banner's typefaces.

The wordmark dogfoods our MIT library `@isonimus/glitch-js`, **vendored** into `web/lib/glitch.es.js`
(unmodified, under a provenance banner) like the parent, but using the **RGB-split** effect rather
than the parent's one-shot decrypt, to echo the banner title's chromatic aberration: a subtle
continuous split on the wordmark only. Progressive enhancement holds (the real text `WiFi Sapper` is
in the HTML; the effect animates an inner span, so a failed module or JS-off renders static and
legible), and it is gated off under `prefers-reduced-motion`. glitch-js is a page asset, not a
firmware dependency.

### What was considered and not reused

- A single auto-`chipFamily` manifest with one entry per family (the parent's shape): rejected,
  because two boards share each family and it would flash the wrong sibling (the delta above).
- `releases/latest/download/...` absolute URLs in the manifest: rejected for the in-browser flow by
  the parent's CORS finding (no ACAO header on release assets); kept valid only for CLI/M5Burner
  users, so the same-origin pull is the browser path.
- Hotlinked Google Fonts / CDN glitch-js: rejected to preserve self-containment; both are vendored.
- A native unit test: none, as with the parent and the other static-asset/CI slices. There is no
  application logic to exercise; correctness is valid JSON, a well-formed workflow, a verified pin,
  and a real device flash.

## Verification

No `scripts/*-verify.mjs`: this slice is static assets + CI configuration, so there is no runtime
behaviour a Node verify can assert (the same rationale as adversary:slice-0016). The proof is split:

- **Local (the ceiling here):** each `web/manifest-*.json` is valid JSON in the ESP Web Tools shape
  (`builds[].chipFamily`, `parts[].path`/`offset`); `web/index.html` renders the full design offline
  (only the live flash needs HTTPS + Web Serial + a device); the esp-web-tools version pin is
  confirmed current at build time; `pages.yml` is a well-formed first-party Pages deploy with
  least-privilege `pages: write` + `id-token: write`; the `release.yml` change adds the four
  stable-named assets and the `pages.yml` dispatch without altering the ADR-0095-validated merge or
  its bootloader guard; `npm run lint` + `npm run index` stay green.
- **Remote / real run (deliberate future acts, tracked in LEDGER until observed):** (1) enable Pages
  (Source = GitHub Actions) and confirm `pages.yml` deploys `web/`; (2) on the live HTTPS page in
  desktop Chrome, flash each of the four boards and confirm each boots its firmware; (3) confirm the
  next `v*` release publishes the four `sapper-<env>-factory.bin` assets and that the dispatched
  Pages redeploy serves them.

## As built

The static assets and CI wiring shipped as designed; the local half of `## Verification` is green. The
remote half (Pages enabled, a real browser flash of each board, the next release's cascade) stays an
operator act tracked in the LEDGER, so this section is **not yet frozen** and is completed there.

What shipped:

- `web/index.html` — self-contained, inline CSS, banner-matched cyan/magenta duotone on near-black. A
  board `<select>` (defaulting to the reference Cardputer) swaps the single `<esp-web-install-button>`'s
  `manifest` attribute and the spec readout on change; a plain non-module script so it runs even if the
  glitch module fails. The wordmark `WiFi Sapper` is real text with a continuous, subtle RGB-split from
  the vendored glitch-js (`Effects.rgbSplit`, `maxOffset: 3`, `frequency: 0.12`), progressive-enhanced
  and `prefers-reduced-motion`-gated. The unsupported and not-allowed slots route Firefox/Safari/mobile
  and non-HTTPS to the manual `esptool` fallback. The panel note states plainly that install erases the
  whole device including internal LittleFS (captures, cracked manifest, provisioning).
- `web/manifest-{cardputer,t_embed,m5stickc_plus2,cyd_3248s035c}.json` — four single-build manifests,
  real `chipFamily` each (ESP32-S3 / ESP32), relative `./sapper-<env>-factory.bin` at `offset: 0`,
  `new_install_prompt_erase: true`. Validated as ESP Web Tools shape.
- `web/fonts/` — Chakra Petch 700 (wordmark/lead) + Rajdhani 400/600 (body), latin-only woff2, vendored.
- `web/lib/glitch.es.js` — `@isonimus/glitch-js` v2.2.0, vendored unmodified under a provenance banner
  (the latest at build time; the parent's slice-0016 vendored 2.0.0, and the `rgbSplit` API is identical).
- esp-web-tools pinned to `@10.4.0` via unpkg (confirmed latest at build time, 2026-10-29).
- `.github/workflows/pages.yml` — first-party `actions/*` only (`configure-pages` → `upload-pages-artifact`
  → `deploy-pages`), least-privilege `pages: write` + `id-token: write`, `concurrency: pages`. Pulls all
  four stable factory images same-origin via `gh release download`, failing loud on a missing asset.
- `.github/workflows/release.yml` — per board, a `cp` of the dio-guarded factory image to the stable name
  `sapper-<env>-factory.bin` (rides the existing `dist/` artifact and `SHA256SUMS.txt`); the release job
  publishes the four stable names explicitly (not via the versioned glob, to avoid a double-upload), gains
  `actions: write`, and ends by dispatching `pages.yml` (the GITHUB_TOKEN release cannot cascade it). The
  ADR-0095 merge and its bootloader dio/offset guard are untouched.
- `.gitignore` — `web/*.bin` (deploy-time only); README gains a "Flash from your browser" section linking
  the hosted page.

Deviations from the design: none of substance. The design said "vendor glitch-js pinned to a confirmed
version"; the confirmed-latest turned out to be 2.2.0 rather than the parent's 2.0.0, vendored after
confirming `Effects.rgbSplit`'s option names are unchanged. The repo slug is `Isonimus/wifi-sapper` (not
`esp32-wifi-sapper`), so all page/README links and the Pages URL use that.
