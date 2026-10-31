# Changelog

Notable shipped changes, newest first. A **hand-maintained** operator-facing record in the
style of the parent Adversary's changelog: each entry states the user- or dev-visible change in
plain language, with the slice and/or ADR that shipped it in parentheses. It complements the
per-tag GitHub Releases (the machine record of binaries and checksums, cut by `release.yml` on
each `v*` tag, ADR-0095); this file is the curated human narrative. A live document
(stele:ADR-0010), updated in the same change that ships a user- or dev-facing feature, and a new
`## <date> (vX.Y.Z)` block is added when a version is tagged.

---

## Unreleased

- **The web flasher has a favicon.** The browser tab now shows the WiFi Sapper signal glyph (cyan
  arcs rising from a magenta emitter dot) as a scalable SVG, echoing the logo and the on-device HUD,
  instead of the browser's blank-page default.

## 2026-10-31 (v0.2.0)

- **The M5Stack Atom Echo is a supported board.** A fifth target joins the lineup, headless: the
  ESP32-PICO-D4 Atom Echo (no display, no PSRAM, 4 MB flash) runs the full hunt, upload and
  cracked-sync spine with its single RGB LED and side button as the only surfaces. It is the first
  board to need **no new decision** to add, a pure recombination of capability axes already in the
  tree (headless like the CYD, internal-heap cracked manifest like the CYD, button Maintenance entry
  like the M5StickC Plus2, RGB LED like the Cardputer), and the first to prove the ADR-0082
  internal-heap manifest and the wpa-sec cracked-download seed working **together** on no-PSRAM
  classic silicon. (slice-0105; ADR-0082, ADR-0078, ADR-0074)

Internal: the Atom Echo pins `upload_speed = 115200` because its PICO-D4 USB-UART bridge drops at
esptool's 460800 write-baud switch. The web flasher gained its `atom_echo` option and factory-image
manifest, and `release.yml` a fifth packaging leg (classic-ESP32 `0x1000` dio guard, §4 #42).

## 2026-10-30 (v0.1.0)

First tagged release: the autonomous appliance and its first four boards. Curated highlights of
what shipped, grouped by theme rather than enumerated per slice.

- **Endless autonomous handshake hunt.** The device hops channels, discovers access points from
  their beacons, captures the WPA/WPA2 four-way handshake, and never stops: there is no "done" state,
  it just keeps hunting. (slices 0010-0018; ADR-0009, ADR-0011, ADR-0015)
- **Auto-upload to wpa-sec, and hourly cracked-results sync.** Each new capture is uploaded to
  wpa-sec.stanev.org, and once an hour the device fetches the account's cracked results back and
  announces any newly recovered passwords. (slices 0018, 0020; ADR-0017, ADR-0019)
- **Headless-first, runs with or without a screen.** The capture-upload-sync engine has no UI
  dependency; every surface is an event-bus subscriber. If a panel fails to initialise at boot the
  unit logs `[FATAL] display init failed` and keeps hunting fully headless rather than stalling.
  (slice-0046; ADR-0001, ADR-0021, ADR-0045)
- **Four supported boards.** M5Stack Cardputer / ADV (the reference), M5StickC Plus2, LilyGo
  T-Embed, and the ESP32-3248S035C "CYD", spanning ESP32-S3 and classic ESP32, PSRAM and no-PSRAM,
  sprite-backed and direct-to-panel displays, and a headless profile. (slices 0005, 0079, 0081, 0083,
  0085, 0087; ADR-0002, ADR-0080, ADR-0082, ADR-0084, ADR-0086)
- **Deauth is off by default and operator-armed.** The raw-TX capability ships in every binary but
  transmits nothing unless the operator explicitly arms deauth in provisioning, so a lost or misused
  unit cannot jam by default. (slices 0028, 0030; ADR-0027, ADR-0029)
- **Local and off-device surfaces.** A status LED, an on-device hunt HUD showing the network being
  sniffed with handshake-progress markers, and an optional push webhook (ntfy / Discord) that fires
  on captures and on newly cracked passwords, each notification type individually enabled. No pushed
  payload ever carries a plaintext password. (slices 0022, 0024, 0026, 0032, 0034, 0036; ADR-0023,
  ADR-0025, ADR-0033, ADR-0035)
- **Captive-portal provisioning, no secrets in the image.** A fresh device raises a `Sapper-XXXX`
  setup access point; the setup form stores WiFi credentials, the wpa-sec key and toggles to NVS, and
  renders stored state back without ever echoing a secret. (slices 0007, 0038; ADR-0006, ADR-0037)
- **Maintenance mode: a hardened dashboard of what the unit found.** A deliberate on-device gesture
  (keyboard `m` on the Cardputer, a side-button hold on button boards) reboots into a password-gated
  SoftAP that serves recovered passwords, capture depth and provisioning, then power-cycles back to
  hunting. (slices 0040, 0044, 0054, 0056, 0058; ADR-0039, ADR-0057, ADR-0078)
- **The hunt spends its airtime where it pays.** It skips already-cracked, already-submitted and
  open networks, attempts stronger signals before weaker ones, backs off access points that never
  yield a handshake, and re-verifies a known network on a bounded cadence in case its password
  rotated. (slices 0060-0073; ADR-0059, ADR-0061, ADR-0063, ADR-0065, ADR-0068, ADR-0070, ADR-0072)
- **Battery gauge and inactivity screen dimming.** Boards with a battery divider show a charge level
  on the HUD, and the screen dims then turns off on inactivity, backlight-only: the hunt never pauses
  in the dark. (slices 0092, 0094; ADR-0091, ADR-0093)
- **Product boot faces.** A resolution-aware splash and on-screen first-boot provisioning
  instructions across three panel tiers; the panel-parameter test pattern moved off the shipped boot
  face to a bench probe. (slices 0042, 0090, 0104; ADR-0041, ADR-0089)
- **Install from the browser, from tagged factory images.** A GitHub Pages web flasher (ESP Web
  Tools) flashes any board over USB from the latest tagged release, which `release.yml` packages as
  `dio`-headed factory images with SHA-256 checksums, brick-guarded per chip family. (slices 0096,
  0102; ADR-0095)

Internal: built with the [Stele](https://www.npmjs.com/package/@isonimus/stele) method:
design-first decision records (`adr/`), work-unit slices frozen at merge (`slices/`), one mutable
ledger, executable invariants enforced by a pre-commit hook and CI, and a native Unity test suite.
