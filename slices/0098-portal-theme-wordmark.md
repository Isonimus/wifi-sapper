---
id: '0098'
title: "Restyle the served portal: a dark banner-matching theme and an SVG wordmark on both pages"
type: slice
status: accepted
date: 2026-10-28
supersedes: []
superseded_by: []
---

## Goal

Bring the two served portal pages into the project's visual identity (`docs/images/wifi-sapper-banner.webp`),
the served-HTML half of the pass whose on-panel half was slice-0097. Both pages today render unstyled
(the setup form carries no CSS at all; the Maintenance dashboard carries a three-line `sans-serif`/grey
block). This slice gives both a self-contained dark theme (cyan primary + magenta secondary accents,
system-font typography) and a shared `WiFi Sapper` SVG wordmark: white glyphs over a blurred magenta glow
with a cyan tagline, matching the banner.

This delivers the LEDGER `[feature]` "restyle the served provisioning/maintenance portal" (operator scope:
**Theme + SVG wordmark**). It also subsumes that item's note that the dashboard `<h2>` was the fourth
product-name site: the product name now renders as the SVG wordmark on both pages, so it gets the branded
treatment rather than a bare heading. It does **not** close the separate product-name-constant deferral
(LEDGER): the wordmark literal is one more spelling of the string, not a single `kProductName` source.

Scope is the two pure head-builders only (`net/provisioning_form`, `net/dashboard`). The on-device
Maintenance *panel* (LovyanGFX) is untouched; the engine, routes, and provisioning logic are untouched.

## Definition of Done

- **Given** either portal page is built
- **When** its head-builder renders (`buildSetupForm` / `buildDashboardHead`)
- **Then** the shared theme CSS and the SVG wordmark are each emitted exactly once in that one head chunk:
  the CSS inside `<head>` (before `<body>`), the wordmark heading the `<body>` as the visible product header;
  and the page fits its buffer (fails loud, returning 0, if it ever does not).

- **Given** the Maintenance dashboard with recovered results
- **When** the page streams (head, then one chunk per result, then controls + foot)
- **Then** the theme and wordmark appear only in the head and in **no** per-row chunk (§4 #21 holds: the
  up-to-256 rows are never burdened with the theme), and every existing fact renders unchanged (the summary
  counts, deauth state, sync history, the filesystem `warn` line, the HTML-escaped PSK rows, and the POST
  resume / `/config` controls).

- **Given** the setup form rendered with stored secrets
- **When** `buildSetupForm` renders it
- **Then** every secret input keeps its exact no-echo markup (no `value=` on a secret field, §4 #20): the
  theme is applied by tag/type CSS selectors and a wrapping `<div>`, never by a class added to a field.

- **Given** an operator viewing either page in a browser on the SoftAP (which has no uplink)
- **When** the page loads
- **Then** it renders the dark banner theme (cyan/magenta accents, system font) and the white-glyph /
  magenta-glow `WiFi Sapper` wordmark with its cyan tagline, fully self-contained (no external fetch).

## Design

**Existing code considered.** The two builders are the pure, host-tested sites the portal already renders
through (ADR-0037, ADR-0039); the restyle lives there so §4 #20 (no secret reaches served HTML) and §4 #21
(the dashboard streams, never materialises the whole page) are preserved by construction, not by review.
The dashboard already carried a tiny inline `<style>`; it is replaced, not extended. The setup form's
field markup is reused verbatim: the no-echo tests pin each `<input>`'s exact bytes (e.g.
`name='pass' type='password' maxlength='63'>`), so the theme must style inputs by **tag/type selector**
(`input[type=password]`, `input:not([type])`, `button`, `table`), never by a class on the field, and only a
wrapping `<div class='wrap'>` and the header are added.

**One shared asset, not two copies (`net/portal_theme.*`).** The theme CSS and the wordmark SVG are owned
once and injected into both builders via `%s`. This is a shared *asset* (static bytes), not duplicated
*logic*, so the rule-of-three (which governs logic, §3) does not call for a per-page copy; a second copy is
exactly the drift hazard that would let the two pages' brand diverge on the next edit. The module adds no
NVS or secret-access site (it is pure data handed to the renderers), so #20/#21 are untouched. It stays a
leaf data module, the sibling of slice-0097's `surface/wordmark` leaf helper.

**Self-contained by necessity (measure the constraint).** The provisioning SoftAP has no uplink, so a web
font or hosted stylesheet would render unstyled on the operator's phone. The theme uses only a system-font
stack and the wordmark is inline SVG: zero network fetches. Colours are CSS custom properties so the
palette lives in one `:root` block (near-black `#0b0b12`, cyan `#35e6ff`, magenta `#c558ff`).

**The wordmark exceeds the panel's emulation.** slice-0097 drew the on-panel wordmark as white glyphs over
a *hard* offset magenta shadow because the `IDisplay` seam cannot blur (ADR-0025). A browser can: the SVG
uses `feDropShadow` with `stdDeviation` blur, so the served page renders the true soft magenta glow the
banner shows, landing closer to the banner than the panel does. White glyphs, cyan tagline, as on the panel.

**Buffers sized from measurement (§4).** Adding the shared head assets (CSS **1620 B** + SVG **621 B**,
measured) grows both pages. Measured worst cases: the setup form (every "leave blank to keep" placeholder +
both dim fields at 65535) renders **4195 B**, the dashboard head (the reformat-warning line) **2859 B**. So
`kSetupFormBufSize` 2048 -> **5120** (~900 B margin) and `kDashboardHeadBufSize` 2048 -> **4096** (~1.2 KB
margin); each still a fixed bound where an overflow is a real bug, and each builder still fails loud
(returns 0, empties `out`) rather than truncate. All call sites take the buffer from these constants, so the
device stack buffers grow with them.

## Verification

The pure builders' *structure* is machine-checkable; the *appearance* is visual (like slice-0097's panel
wordmark), so the proof is split:

- **Unit tests (machine-checked), DoD scenarios 1-3:**
  - `test/test_dashboard`: `test_head_carries_the_theme_and_wordmark_in_the_head` (both assets emitted and
    before `<body>`), `test_row_stream_carries_no_theme_bytes` (no `<style>`/`<svg>` in a result row, §4
    #21), and every pre-existing head/row/control/escape/fail-loud test still green (facts unchanged).
  - `test/test_provisioning_form`: `test_form_carries_the_theme_and_wordmark_in_the_head` (worst-case model
    still renders and fits 5120), and the full no-echo suite still green
    (`test_form_never_prefills_a_secret_value`, the exact-input-markup asserts) proving the field bytes and
    #20 are unchanged by the restyle.
- **Visual (operator-run), DoD scenario 4:** open both pages in a phone/desktop browser served from the
  device SoftAP with no uplink: the setup form (first boot / STA-fail fallback) and the Maintenance
  dashboard (BOOT-hold entry). Confirm the dark theme, cyan/magenta accents, the white-glyph / magenta-glow
  wordmark + cyan tagline, legible inputs and results table, and that nothing fails to load (self-contained).
  A pre-flight render of both pages to an HTML file (opened in a desktop browser) can preview the look
  before flashing.
- **No new wired `scripts/*-verify.mjs`:** the device serve/stream path is unchanged (only the static HTML
  content changed, and its structure is now unit-asserted); the existing `scripts/0007-provisioning-verify`
  and `scripts/0040-maintenance-verify` error-gates (no `[FATAL]`, portal serves) still cover the serve path.
- **Native suite + four-board builds (machine-checked):** `npm run test:native` and the per-board compiles
  stay green; `net/portal_theme.cpp` is added to the native `build_src_filter`.

## As built

Shipped as designed: a single shared asset `net/portal_theme.*` owns the dark banner theme CSS and the
`WiFi Sapper` SVG wordmark (white glyphs over a blurred magenta glow + cyan tagline), injected via `%s`
into both pure head-builders (`buildSetupForm`, `buildDashboardHead`). The setup form's field markup is
unchanged (styled by tag/type selector + a `<div class='wrap'>`), so the §4 #20 no-echo tests stay green;
the dashboard still streams its rows separately with the theme in the head only (§4 #21).

**Tuned with the operator.** The submit button shipped **plain cyan** (near-black text), not the initial
magenta→cyan gradient (operator preference after the desktop preview). While restyling, a leaked internal
citation `(ADR-0039)` was found in the served dashboard warning copy and removed (boyscout): the rationale
stays in the code comment and this slice, not on the operator-facing page.

**Buffers sized from measurement.** CSS 1620 B + SVG 621 B. Worst-case renders: setup form 4195 B (buffer
2048 → 5120, ~900 B margin), dashboard head 2859 B (buffer 2048 → 4096, ~1.2 KB margin). Both still fail
loud (return 0, empty `out`) rather than truncate; every call site sizes its stack buffer from the constant.

**Machine-checked.** Lint 0/0, `adr/INDEX.md` regenerated, native suite **376/376** (was 373; +3:
`test_head_carries_the_theme_and_wordmark_in_the_head`, `test_form_carries_the_theme_and_wordmark_in_the_head`,
and `test_row_stream_carries_no_theme_bytes` — the §4 #21 guard that the theme never rides a result row).
All four envs build clean (`cardputer`, `m5stickc_plus2`, `t_embed`, `cyd_3248s035c`); no `.dram0.bss`
overflow on the two classic-ESP32 boards. DoD scenarios 1-3 are these unit tests.

**Visual proof (operator-confirmed, 2026-10-29), DoD scenario 4.** Both complete pages were rendered to
HTML (the exact served bytes) and opened in a desktop browser: the operator confirmed the dark theme,
cyan/magenta accents, and the white-glyph / magenta-glow wordmark with cyan tagline, then requested the
plain-cyan button, which was applied and re-previewed. The on-device SoftAP view is the identical byte
stream (the serve path is unchanged, covered by the `scripts/0007` and `scripts/0040` error-gates); no new
wired verify script, as the runtime path did not change.
