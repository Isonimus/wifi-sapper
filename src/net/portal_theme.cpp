/**
 * @file portal_theme.cpp
 * @brief The shared portal theme CSS + SVG wordmark (slice-0098). Pure data; no Arduino, no hardware.
 */
#include "net/portal_theme.h"

namespace sapper {

// A single `<style>` block, minified. Colours are CSS custom properties so the palette lives in one place:
// near-black background, light text, cyan primary accent (headings, focus, links, table headers, code) and
// magenta secondary accent (checkboxes, the submit gradient, the .warn lines). The `.warn` class keeps the
// name the dashboard's filesystem/PSK notices already use (ADR-0074), recoloured from the old red #a00.
// Inputs are styled by tag/type selector, never by a class on the field, so the setup form's exact input
// markup (which the no-echo tests pin, §4 #20) is untouched by the restyle.
const char kPortalThemeCss[] =
    "<style>"
    ":root{--bg:#0b0b12;--panel:#15151f;--fg:#e9e9f2;--muted:#9a9ab0;"
    "--cyan:#35e6ff;--mag:#c558ff;--line:#2a2a3a}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);line-height:1.5;"
    "font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif}"
    ".wrap{max-width:30rem;margin:0 auto;padding:1.25rem}"
    ".logo{display:block;width:100%;max-width:17rem;height:auto;margin:.25rem auto 1.25rem}"
    "h2{color:var(--cyan);font-size:1.1rem;font-weight:600;letter-spacing:.03em;"
    "text-align:center;margin:0 0 1.25rem}"
    "p{margin:.7rem 0}small{color:var(--muted)}"
    "input,button{font-family:inherit;font-size:1rem}"
    "input[type=text],input[type=password],input[type=url],input[type=number],input:not([type])"
    "{width:100%;margin-top:.3rem;padding:.55rem .6rem;border:1px solid var(--line);"
    "border-radius:.4rem;background:var(--panel);color:var(--fg)}"
    "input:focus{outline:none;border-color:var(--cyan);box-shadow:0 0 0 2px rgba(53,230,255,.25)}"
    "input[type=checkbox]{width:auto;accent-color:var(--mag);vertical-align:middle}"
    "fieldset{border:1px solid var(--line);border-radius:.5rem;margin:1rem 0;padding:.5rem .8rem}"
    "legend{color:var(--cyan);padding:0 .4rem}"
    "button{width:100%;margin-top:.6rem;padding:.7rem;border:0;border-radius:.5rem;cursor:pointer;"
    "font-weight:700;color:#0b0b12;background:var(--cyan)}"
    "a{color:var(--cyan)}ul{padding-left:1.1rem}"
    "hr{border:0;border-top:1px solid var(--line);margin:1.1rem 0}"
    "table{border-collapse:collapse;width:100%;margin-top:.5rem}"
    "th,td{border:1px solid var(--line);padding:.45rem .5rem;text-align:left}"
    "th{color:var(--cyan)}code{word-break:break-all;color:var(--cyan)}"
    ".warn{color:var(--mag)}"
    "</style>";

// White wordmark over a blurred magenta glow (the soft neon the panel's hard offset shadow emulated,
// slice-0097), cyan tagline beneath. viewBox-scaled so the CSS `.logo{max-width}` sizes it responsively.
// Attribute values use single quotes (like the rest of the served HTML); the font stack carries no
// quoted family name, so the SVG needs no escaped double quotes.
const char kPortalWordmarkSvg[] =
    "<svg class='logo' viewBox='0 0 320 66' role='img' aria-label='WiFi Sapper' "
    "xmlns='http://www.w3.org/2000/svg'>"
    "<defs><filter id='glow' x='-15%' y='-60%' width='130%' height='220%'>"
    "<feDropShadow dx='0' dy='0' stdDeviation='3.5' flood-color='#c558ff' flood-opacity='.9'/>"
    "</filter></defs>"
    "<text x='160' y='40' text-anchor='middle' filter='url(#glow)' fill='#ffffff' "
    "font-family='system-ui,Roboto,sans-serif' font-weight='800' font-size='33'>WiFi Sapper</text>"
    "<text x='160' y='58' text-anchor='middle' fill='#35e6ff' "
    "font-family='system-ui,sans-serif' font-size='8.5' letter-spacing='3'>"
    "AUTONOMOUS HANDSHAKE HUNTER</text>"
    "</svg>";

}  // namespace sapper
