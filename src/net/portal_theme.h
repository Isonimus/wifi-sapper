/**
 * @file portal_theme.h
 * @brief The shared visual identity for the two served portal pages (slice-0098). Host-tested; pure data.
 *
 * The captive-setup form (net/provisioning_form, ADR-0037) and the Maintenance dashboard (net/dashboard,
 * ADR-0039) are two separate pure head-builders, but they present one product. This module owns the one
 * thing that must look byte-identical on both: the dark banner-matching theme CSS and the `WiFi Sapper`
 * SVG wordmark. Both builders inject these as `%s` into their single fail-loud `snprintf`, so the assets
 * ride the once-served head and never the dashboard's per-row stream (§4 #21 holds by construction).
 *
 * WHY a shared constant and not a copy in each builder: this is a single shared *asset* (static bytes),
 * not duplicated *logic* — so the rule-of-three (which governs logic) does not call for a copy here; a
 * second copy is exactly the drift hazard that would let the two pages' brand diverge on the next edit.
 * It introduces no new NVS or secret-access site (it is pure string data handed to the renderers), so the
 * no-echo (#20) and streamed-not-materialised (#21) guarantees are untouched.
 *
 * WHY fully self-contained (no external font/stylesheet/CDN): the provisioning SoftAP has no uplink, so a
 * page that referenced a web font or a hosted stylesheet would render unstyled on the operator's phone.
 * The theme uses only a system-font stack, and the wordmark is an inline SVG — no network fetch.
 *
 * The wordmark renders white glyphs over a true *blurred* magenta glow (an SVG `feDropShadow`), the soft
 * neon the on-panel wordmark could only approximate with a hard offset shadow (slice-0097): the IDisplay
 * seam cannot blur, a browser can, so the served page lands closer to the banner than the panel does.
 */
#pragma once

namespace sapper {

/// The theme stylesheet as a complete `<style>...</style>` block (dark background, cyan/magenta accents,
/// system-font typography). Injected into the head of both portal pages. Covers every element both pages
/// use (form inputs, fieldset, button, table, code, .warn) so the one block themes both.
extern const char kPortalThemeCss[];

/// The `WiFi Sapper` wordmark as an inline `<svg>`: white glyphs over a blurred magenta glow, with the
/// cyan tagline beneath, matching the project banner. Carries its own `aria-label`. Placed at the top of
/// both pages as the product header, so the product name gets the branded treatment rather than a bare
/// `<h2>` (this is the served-HTML product-name site, the sibling of slice-0097's on-panel wordmark).
extern const char kPortalWordmarkSvg[];

}  // namespace sapper
