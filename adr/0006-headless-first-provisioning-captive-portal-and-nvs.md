---
id: '0006'
title: "Headless-first provisioning: a first-boot captive portal, secrets in NVS"
type: architecture
status: accepted
date: 2026-09-18
supersedes: []
superseded_by: []
---

## Context

The Sapper needs two secrets before it can do anything useful: **WiFi station credentials**
(to reach NTP and `wpa-sec.stanev.org` over TLS) and the **wpa-sec API key** (to upload
handshakes and fetch cracked results). Slice-2 provisions both. The question this ADR settles
is *how a secret gets onto a device that may have no keyboard, no screen, and no removable
storage* — and *where it lives at rest*.

The parent Adversary answers neither question in a way the Sapper can inherit. Its
`SettingsManager` (`../adversary/src/modules/storage/settings_manager.h`) stores config as
JSON on **SD** (M5Stack) or **LittleFS** (M5Stick), reads API keys from
`/adversary/config/wpasec.txt`, and takes the key and WiFi password through its **on-screen
keyboard** (`TextInputPopup`, `wpaSecKeyPopup_`). Every one of those paths assumes the
interactive, screen-and-keyboard device ADR-0001 deliberately walked away from. A bare ESP32
or a screenless build has no keyboard to type a key into and no SD slot to drop a file on.

Two forces constrain the answer:

- **Universality.** ADR-0001 made every surface (screen, keyboard, SD, LED) optional — the
  capture→upload→sync spine must run on hardware that has none of them. The one input every
  target board *does* have is a WiFi radio. A provisioning channel that depends on any
  optional surface fails the ADR-0001 contract.
- **The serial channel cannot be the provisioning path.** ADR-0003 keeps the shipped serial
  vocabulary **observation-only** precisely so no fielded Sapper can be commanded over USB.
  A `setwifi`/`setkey` serial command would be exactly the persistent-state actuation that
  invariant #4 forbids in a non-`SAPPER_TEST_HOOKS` build — and it would require a USB host,
  which defeats a drop-and-go appliance. Provisioning must live elsewhere.

ADR-0003 §7 already anticipated this ADR: it declared that "the first-boot captive-portal
provisioning state (its own forthcoming ADR) is a pre-engine mode a freshly-flashed device
sits in, and the harness must be able to drive a just-provisioned device." This is that ADR,
and it inherits that obligation.

## Decision

**1. Primary provisioning is a first-boot captive AP portal.** On a boot with no stored
credentials, the Sapper raises a SoftAP, hijacks DNS, and serves a one-page web form
(SSID + password + wpa-sec key). The operator connects a phone or laptop and fills it in —
**the client device supplies the screen and keyboard the Sapper may not have**, which is what
makes this the one path that works on every board. This reuses the Arduino `DNSServer` +
`WebServer` plumbing the Adversary already proved (`../adversary/src/modules/ap/arduino_captive.h`),
not its UI-coupled offensive portal.

**2. Secrets live in NVS, not a file.** Provisioned values are written to the ESP-IDF NVS
(`Preferences`) under a `sapper` namespace (`wifi_ssid`, `wifi_pass`, `wpasec_key`). NVS is
the right home because it needs no SD assumption, survives an ordinary reflash, and is the
platform-standard store for WiFi credentials. **NVS is plaintext at rest** unless flash
encryption is enabled; pulling the flash reveals the key. That is an accepted limitation for
now — flash encryption is a separate, heavier decision (secure-boot key management, one-way
fusing) recorded as a ledger item, not smuggled into this slice.

**3. Boot is a two-way gate.** Valid credentials in NVS → straight to STA + NTP + engine,
with **no human in the loop** — the headless steady state. Absent or invalid credentials, or
STA association failing a bounded number of times, → the portal. A device that cannot reach
its configured network re-opens for reconfiguration rather than silently looping (quality-bar
§3: fail loud, no fallback that masks a real failure).

**4. The portal pumps the serial channel.** The portal is a pre-engine blocking state, so per
ADR-0003 §7 and CLAUDE.md §4 invariant #3 it MUST drain and answer the observation channel
while it waits. `state` reports `phase=provisioning` so the verify harness can observe a
freshly-flashed, unprovisioned device and confirm it is alive and awaiting input.

**5. The SoftAP is discoverable without a screen.** Its SSID is `Sapper-XXXX`, where `XXXX`
is the last two bytes of the station MAC, with a documented default password. A build with a
screen or LED also announces it, but the naming convention means a screenless device is still
provisionable by an operator who has read the README — which documents the name pattern,
default password, and the portal URL as the live dev-facing entry point (stele:ADR-0010).

**6. Re-provisioning without a serial actuation path.** Credentials are cleared by a
GPIO/button hold at boot **on profiles that declare an input** (`BoardProfile`), and by the
association-failure fallback of decision #3 on those that do not. No serial command clears or
writes them — ADR-0003's shipped-binary invariant stays intact.

**7. The verify seam is NVS, not the web form.** The portal's HTTP handler validates and
parses the form, then commits through a single `persistProvisioning(creds)` seam. Under
`SAPPER_TEST_HOOKS` the harness injects credentials through that *same* seam (ADR-0003
decision #3: stimulus enters through the seam a real event uses), so a device verify can drive
a provisioned boot without a browser and without a parallel test-only path.

## Consequences

- NTP and every wpa-sec call depend on STA coming up, which depends on provisioning — so the
  portal is on the critical path to the very first capture upload. Its correctness is
  load-bearing, which is why decision #7 makes it headlessly verifiable rather than
  eyeball-once.
- This ADR adds standing invariants to CLAUDE.md §4 in the same commit that accepts it:
  (a) secrets are read from and written to NVS through the `persistProvisioning` seam only,
  never scattered across call sites; (b) no provisioning or credential-clearing capability
  exists in the serial vocabulary of any build. Invariant #3 (pre-engine states pump the
  channel) gains its first concrete instance here and moves from pending to verified once
  slice-2's device verify exercises it.
- The Adversary's SD/LittleFS config-file path is **not** carried over as a primary channel.
  A power-user "seed NVS from a file on first boot, then delete it" convenience is possible on
  profiles that have storage, but it is deferred to keep slice-2 to one provisioning path
  (KISS); a ledger item holds the option.
- Plaintext-at-rest key storage is a real exposure for a device that may be left unattended in
  the field. Recorded as a ledger item for a future flash-encryption/secure-boot ADR, so the
  decision to defer is visible rather than implicit.
- The SoftAP + captive portal is itself a transmit surface, but only while unprovisioned,
  only SoftAP-local, and it carries no attack vocabulary — it is a config form, distinct from
  the deauth/upload paths ADR-0003 gates.
